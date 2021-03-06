// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <rdma/rsocket.h>
#include <netinet/tcp.h>
#include <sys/uio.h>
#include <limits.h>
#include <poll.h>

#include "Message.h"
#include "Pipe.h"
#include "SimpleMessenger.h"

#include "common/debug.h"
#include "common/errno.h"

// Below included to get encode_encrypt(); That probably should be in Crypto.h, instead

#include "auth/Crypto.h"
#include "auth/cephx/CephxProtocol.h"
#include "auth/AuthSessionHandler.h"

// Constant to limit starting sequence number to 2^31.  Nothing special about it, just a big number.  PLR
#define SEQ_MASK  0x7fffffff 
#define dout_subsys ceph_subsys_ms

#undef dout_prefix
#define dout_prefix _pipe_prefix(_dout)
ostream& Pipe::_pipe_prefix(std::ostream *_dout) {
  return *_dout << "-- " << msgr->get_myinst().addr << " >> " << peer_addr << " pipe(" << this
		<< " sd=" << sd << " :" << port
		<< " s=" << state
		<< " pgs=" << peer_global_seq
		<< " cs=" << connect_seq
		<< " l=" << policy.lossy
		<< ").";
}



/**************************************
 * Pipe
 */

Pipe::Pipe(SimpleMessenger *r, int st, Connection *con)
  : reader_thread(this), writer_thread(this),
    delay_thread(NULL),
    msgr(r),
    conn_id(r->dispatch_queue.get_id()),
    sd(-1), port(0),
    peer_type(-1),
    pipe_lock("SimpleMessenger::Pipe::pipe_lock"),
    state(st),
    session_security(NULL),
    connection_state(NULL),
    reader_running(false), reader_needs_join(false),
    writer_running(false),
    in_q(&(r->dispatch_queue)),
    keepalive(false),
    close_on_empty(false),
    connect_seq(0), peer_global_seq(0),
    out_seq(0), in_seq(0), in_seq_acked(0) {
  if (con) {
    connection_state = con->get();
    connection_state->reset_pipe(this);
  } else {
    connection_state = new Connection(msgr);
    connection_state->pipe = get();
  }

  if (randomize_out_seq()) {
    lsubdout(msgr->cct,ms,15) << "Pipe(): Could not get random bytes to set seq number for session reset; set seq number to " << out_seq << dendl;
  }
    

  msgr->timeout = msgr->cct->_conf->ms_tcp_read_timeout * 1000; //convert to ms
  if (msgr->timeout == 0)
    msgr->timeout = -1;
}

Pipe::~Pipe()
{
  assert(out_q.empty());
  assert(sent.empty());
  if (connection_state)
    connection_state->put();
  delete session_security;
  delete delay_thread;
}

void Pipe::handle_ack(uint64_t seq)
{
  lsubdout(msgr->cct, ms, 15) << "reader got ack seq " << seq << dendl;
  // trim sent list
  while (!sent.empty() &&
	 sent.front()->get_seq() <= seq) {
    Message *m = sent.front();
    sent.pop_front();
    lsubdout(msgr->cct, ms, 10) << "reader got ack seq "
				<< seq << " >= " << m->get_seq() << " on " << m << " " << *m << dendl;
    m->put();
  }

  if (sent.empty() && close_on_empty) {
    lsubdout(msgr->cct, ms, 10) << "reader got last ack, queue empty, closing" << dendl;
    stop();
  }
}

void Pipe::start_reader()
{
  assert(pipe_lock.is_locked());
  assert(!reader_running);
  if (reader_needs_join) {
    reader_thread.join();
    reader_needs_join = false;
  }
  reader_running = true;
  reader_thread.create(msgr->cct->_conf->ms_rwthread_stack_bytes);
}

void Pipe::maybe_start_delay_thread()
{
  if (!delay_thread &&
      msgr->cct->_conf->ms_inject_delay_type.find(ceph_entity_type_name(connection_state->peer_type)) != string::npos) {
    lsubdout(msgr->cct, ms, 1) << "setting up a delay queue on Pipe " << this << dendl;
    delay_thread = new DelayedDelivery(this);
    delay_thread->create();
  }
}

void Pipe::start_writer()
{
  assert(pipe_lock.is_locked());
  assert(!writer_running);
  writer_running = true;
  writer_thread.create(msgr->cct->_conf->ms_rwthread_stack_bytes);
}

void Pipe::join_reader()
{
  if (!reader_running)
    return;
  cond.Signal();
  pipe_lock.Unlock();
  reader_thread.join();
  pipe_lock.Lock();
  reader_needs_join = false;
}

void Pipe::DelayedDelivery::discard()
{
  lgeneric_subdout(pipe->msgr->cct, ms, 20) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::discard" << dendl;
  Mutex::Locker l(delay_lock);
  while (!delay_queue.empty()) {
    Message *m = delay_queue.front().second;
    pipe->msgr->dispatch_throttle_release(m->get_dispatch_throttle_size());
    m->put();
    delay_queue.pop_front();
  }
}

void Pipe::DelayedDelivery::flush()
{
  lgeneric_subdout(pipe->msgr->cct, ms, 20) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::flush" << dendl;
  Mutex::Locker l(delay_lock);
  while (!delay_queue.empty()) {
    Message *m = delay_queue.front().second;
    delay_queue.pop_front();
    pipe->in_q->enqueue(m, m->get_priority(), pipe->conn_id);
  }
}

void *Pipe::DelayedDelivery::entry()
{
  Mutex::Locker locker(delay_lock);
  lgeneric_subdout(pipe->msgr->cct, ms, 20) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::entry start" << dendl;

  while (!stop_delayed_delivery) {
    if (delay_queue.empty()) {
      lgeneric_subdout(pipe->msgr->cct, ms, 30) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::entry sleeping on delay_cond because delay queue is empty" << dendl;
      delay_cond.Wait(delay_lock);
      continue;
    }
    utime_t release = delay_queue.front().first;
    if (release > ceph_clock_now(pipe->msgr->cct)) {
      lgeneric_subdout(pipe->msgr->cct, ms, 10) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::entry sleeping on delay_cond until " << release << dendl;
      delay_cond.WaitUntil(delay_lock, release);
      continue;
    }
    Message *m = delay_queue.front().second;
    lgeneric_subdout(pipe->msgr->cct, ms, 10) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::entry dequeuing message " << m << " for delivery, past " << release << dendl;
    delay_queue.pop_front();
    pipe->in_q->enqueue(m, m->get_priority(), pipe->conn_id);
  }
  lgeneric_subdout(pipe->msgr->cct, ms, 20) << pipe->_pipe_prefix(_dout) << "DelayedDelivery::entry stop" << dendl;
  return NULL;
}

int Pipe::accept()
{
  ldout(msgr->cct,10) << "accept" << dendl;

  set_socket_options();

  // my creater gave me sd via accept()
  assert(state == STATE_ACCEPTING);
  
  // announce myself.
  int rc = tcp_write(CEPH_BANNER, strlen(CEPH_BANNER));
  if (rc < 0) {
    ldout(msgr->cct,10) << "accept couldn't write banner" << dendl;
    state = STATE_CLOSED;
    state_closed.set(1);
    return -1;
  }

  // and my addr
  bufferlist addrs;
  ::encode(msgr->my_inst.addr, addrs);

  port = msgr->my_inst.addr.get_port();

  // and peer's socket addr (they might not know their ip)
  entity_addr_t socket_addr;
  socklen_t len = sizeof(socket_addr.ss_addr());
  int r = ::rgetpeername(sd, (sockaddr*)&socket_addr.ss_addr(), &len);
  if (r < 0) {
    char buf[80];
    ldout(msgr->cct,0) << "accept failed to getpeername " << errno << " " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    state = STATE_CLOSED;
    state_closed.set(1);
    return -1;
  }
  ::encode(socket_addr, addrs);

  rc = tcp_write(addrs.c_str(), addrs.length());
  if (rc < 0) {
    ldout(msgr->cct,10) << "accept couldn't write my+peer addr" << dendl;
    state = STATE_CLOSED;
    state_closed.set(1);
    return -1;
  }

  ldout(msgr->cct,1) << "accept sd=" << sd << " " << socket_addr << dendl;
  
  // identify peer
  char banner[strlen(CEPH_BANNER)+1];
  if (tcp_read(banner, strlen(CEPH_BANNER)) < 0) {
    ldout(msgr->cct,10) << "accept couldn't read banner" << dendl;
    state = STATE_CLOSED;
    state_closed.set(1);
    return -1;
  }
  if (memcmp(banner, CEPH_BANNER, strlen(CEPH_BANNER))) {
    banner[strlen(CEPH_BANNER)] = 0;
    ldout(msgr->cct,1) << "accept peer sent bad banner '" << banner << "' (should be '" << CEPH_BANNER << "')" << dendl;
    state = STATE_CLOSED;
    state_closed.set(1);
    return -1;
  }
  bufferlist addrbl;
  {
    bufferptr tp(sizeof(peer_addr));
    addrbl.push_back(tp);
  }
  if (tcp_read(addrbl.c_str(), addrbl.length()) < 0) {
    ldout(msgr->cct,10) << "accept couldn't read peer_addr" << dendl;
    state = STATE_CLOSED;
    state_closed.set(1);
    return -1;
  }
  {
    bufferlist::iterator ti = addrbl.begin();
    ::decode(peer_addr, ti);
  }

  ldout(msgr->cct,10) << "accept peer addr is " << peer_addr << dendl;
  if (peer_addr.is_blank_ip()) {
    // peer apparently doesn't know what ip they have; figure it out for them.
    int port = peer_addr.get_port();
    peer_addr.addr = socket_addr.addr;
    peer_addr.set_port(port);
    ldout(msgr->cct,0) << "accept peer addr is really " << peer_addr
	    << " (socket is " << socket_addr << ")" << dendl;
  }
  set_peer_addr(peer_addr);  // so that connection_state gets set up
  
  ceph_msg_connect connect;
  ceph_msg_connect_reply reply;
  Pipe *existing = 0;
  bufferptr bp;
  bufferlist authorizer, authorizer_reply;
  bool authorizer_valid;
  uint64_t feat_missing;
  bool replaced = false;
  CryptoKey session_key;

  // this should roughly mirror pseudocode at
  //  http://ceph.newdream.net/wiki/Messaging_protocol
  int reply_tag = 0;
  uint64_t existing_seq = -1;

  // used for reading in the remote acked seq on connect
  uint64_t newly_acked_seq = 0;

  while (1) {
    if (tcp_read((char*)&connect, sizeof(connect)) < 0) {
      ldout(msgr->cct,10) << "accept couldn't read connect" << dendl;
      goto fail_unlocked;
    }


    authorizer.clear();
    if (connect.authorizer_len) {
      bp = buffer::create(connect.authorizer_len);
      if (tcp_read(bp.c_str(), connect.authorizer_len) < 0) {
        ldout(msgr->cct,10) << "accept couldn't read connect authorizer" << dendl;
        goto fail_unlocked;
      }
      authorizer.push_back(bp);
      authorizer_reply.clear();
    }

    ldout(msgr->cct,20) << "accept got peer connect_seq " << connect.connect_seq
	     << " global_seq " << connect.global_seq
	     << dendl;
    
    msgr->lock.Lock();   // FIXME
    if (msgr->dispatch_queue.stop)
      goto shutting_down;

    // note peer's type, flags
    set_peer_type(connect.host_type);
    policy = msgr->get_policy(connect.host_type);
    ldout(msgr->cct,10) << "accept of host_type " << connect.host_type
	     << ", policy.lossy=" << policy.lossy
	     << dendl;

    memset(&reply, 0, sizeof(reply));
    reply.protocol_version = msgr->get_proto_version(peer_type, false);

    // mismatch?
    ldout(msgr->cct,10) << "accept my proto " << reply.protocol_version
	     << ", their proto " << connect.protocol_version << dendl;
    if (connect.protocol_version != reply.protocol_version) {
      reply.tag = CEPH_MSGR_TAG_BADPROTOVER;
      msgr->lock.Unlock();
      goto reply;
    }

    // require signatures for cephx?
    if (connect.authorizer_protocol == CEPH_AUTH_CEPHX) {
      if (peer_type == CEPH_ENTITY_TYPE_OSD ||
	  peer_type == CEPH_ENTITY_TYPE_MDS) {
	if (msgr->cct->_conf->cephx_require_signatures ||
	    msgr->cct->_conf->cephx_cluster_require_signatures) {
	  ldout(msgr->cct,10) << "using cephx, requiring MSG_AUTH feature bit for cluster" << dendl;
	  policy.features_required |= CEPH_FEATURE_MSG_AUTH;
	}
      } else {
	if (msgr->cct->_conf->cephx_require_signatures ||
	    msgr->cct->_conf->cephx_service_require_signatures) {
	  ldout(msgr->cct,10) << "using cephx, requiring MSG_AUTH feature bit for service" << dendl;
	  policy.features_required |= CEPH_FEATURE_MSG_AUTH;
	}
      }
    }

    feat_missing = policy.features_required & ~(uint64_t)connect.features;
    if (feat_missing) {
      ldout(msgr->cct,1) << "peer missing required features " << std::hex << feat_missing << std::dec << dendl;
      reply.tag = CEPH_MSGR_TAG_FEATURES;
      msgr->lock.Unlock();
      goto reply;
    }
    
    msgr->lock.Unlock();

    // Check the authorizer.  If not good, bail out.

    if (!msgr->verify_authorizer(connection_state, peer_type, connect.authorizer_protocol, authorizer, 
				 authorizer_reply, authorizer_valid, session_key) ||
	!authorizer_valid) {
      ldout(msgr->cct,0) << "accept: got bad authorizer" << dendl;
      reply.tag = CEPH_MSGR_TAG_BADAUTHORIZER;
      delete session_security;
      session_security = NULL;
      goto reply;
    } 

    // We've verified the authorizer for this pipe, so set up the session security structure.  PLR

    ldout(msgr->cct,10) << "accept:  setting up session_security." << dendl;

    msgr->lock.Lock();
    if (msgr->dispatch_queue.stop)
      goto shutting_down;

    
    // existing?
    existing = msgr->_lookup_pipe(peer_addr);
    if (existing) {
      existing->pipe_lock.Lock();

      if (connect.global_seq < existing->peer_global_seq) {
	ldout(msgr->cct,10) << "accept existing " << existing << ".gseq " << existing->peer_global_seq
		 << " > " << connect.global_seq << ", RETRY_GLOBAL" << dendl;
	reply.tag = CEPH_MSGR_TAG_RETRY_GLOBAL;
	reply.global_seq = existing->peer_global_seq;  // so we can send it below..
	existing->pipe_lock.Unlock();
	msgr->lock.Unlock();
	goto reply;
      } else {
	ldout(msgr->cct,10) << "accept existing " << existing << ".gseq " << existing->peer_global_seq
		 << " <= " << connect.global_seq << ", looks ok" << dendl;
      }
      
      if (existing->policy.lossy) {
	ldout(msgr->cct,0) << "accept replacing existing (lossy) channel (new one lossy="
	        << policy.lossy << ")" << dendl;
	existing->was_session_reset();
	goto replace;
      }

      ldout(msgr->cct,0) << "accept connect_seq " << connect.connect_seq
			 << " vs existing " << existing->connect_seq
			 << " state " << existing->get_state_name() << dendl;

      if (connect.connect_seq == 0 && existing->connect_seq > 0) {
	ldout(msgr->cct,0) << "accept peer reset, then tried to connect to us, replacing" << dendl;
	if (policy.resetcheck)
	  existing->was_session_reset(); // this resets out_queue, msg_ and connect_seq #'s
	goto replace;
      }

      if (connect.connect_seq < existing->connect_seq) {
	// old attempt, or we sent READY but they didn't get it.
	ldout(msgr->cct,10) << "accept existing " << existing << ".cseq " << existing->connect_seq
			    << " > " << connect.connect_seq << ", RETRY_SESSION" << dendl;
	goto retry_session;
      }

      if (connect.connect_seq == existing->connect_seq) {
	// if the existing connection successfully opened, and/or
	// subsequently went to standby, then the peer should bump
	// their connect_seq and retry: this is not a connection race
	// we need to resolve here.
	if (existing->state == STATE_OPEN ||
	    existing->state == STATE_STANDBY) {
	  ldout(msgr->cct,10) << "accept connection race, existing " << existing
			      << ".cseq " << existing->connect_seq
			      << " == " << connect.connect_seq
			      << ", OPEN|STANDBY, RETRY_SESSION" << dendl;
	  goto retry_session;
	}

	// connection race?
	if (peer_addr < msgr->my_inst.addr ||
	    existing->policy.server) {
	  // incoming wins
	  ldout(msgr->cct,10) << "accept connection race, existing " << existing << ".cseq " << existing->connect_seq
		   << " == " << connect.connect_seq << ", or we are server, replacing my attempt" << dendl;
	  if (!(existing->state == STATE_CONNECTING ||
		existing->state == STATE_WAIT))
	    lderr(msgr->cct) << "accept race bad state, would replace, existing="
			     << existing->get_state_name()
			     << " " << existing << ".cseq=" << existing->connect_seq
			     << " == " << connect.connect_seq
			     << dendl;
	  assert(existing->state == STATE_CONNECTING ||
		 existing->state == STATE_WAIT);
	  goto replace;
	} else {
	  // our existing outgoing wins
	  ldout(msgr->cct,10) << "accept connection race, existing " << existing << ".cseq " << existing->connect_seq
		   << " == " << connect.connect_seq << ", sending WAIT" << dendl;
	  assert(peer_addr > msgr->my_inst.addr);
	  if (!(existing->state == STATE_CONNECTING))
	    lderr(msgr->cct) << "accept race bad state, would send wait, existing="
			     << existing->get_state_name()
			     << " " << existing << ".cseq=" << existing->connect_seq
			     << " == " << connect.connect_seq
			     << dendl;
	  assert(existing->state == STATE_CONNECTING);
	  // make sure our outgoing connection will follow through
	  existing->_send_keepalive();
	  reply.tag = CEPH_MSGR_TAG_WAIT;
	  existing->pipe_lock.Unlock();
	  msgr->lock.Unlock();
	  goto reply;
	}
      }

      assert(connect.connect_seq > existing->connect_seq);
      assert(connect.global_seq >= existing->peer_global_seq);
      if (policy.resetcheck &&   // RESETSESSION only used by servers; peers do not reset each other
	  existing->connect_seq == 0) {
	ldout(msgr->cct,0) << "accept we reset (peer sent cseq " << connect.connect_seq 
		 << ", " << existing << ".cseq = " << existing->connect_seq
		 << "), sending RESETSESSION" << dendl;
	reply.tag = CEPH_MSGR_TAG_RESETSESSION;
	msgr->lock.Unlock();
	existing->pipe_lock.Unlock();
	goto reply;
      }

      // reconnect
      ldout(msgr->cct,10) << "accept peer sent cseq " << connect.connect_seq
	       << " > " << existing->connect_seq << dendl;
      goto replace;
    } // existing
    else if (policy.resetcheck && connect.connect_seq > 0) {
      // we reset, and they are opening a new session
      ldout(msgr->cct,0) << "accept we reset (peer sent cseq " << connect.connect_seq << "), sending RESETSESSION" << dendl;
      msgr->lock.Unlock();
      reply.tag = CEPH_MSGR_TAG_RESETSESSION;
      goto reply;
    } else {
      // new session
      ldout(msgr->cct,10) << "accept new session" << dendl;
      existing = NULL;
      goto open;
    }
    assert(0);

  retry_session:
    reply.tag = CEPH_MSGR_TAG_RETRY_SESSION;
    reply.connect_seq = existing->connect_seq + 1;
    existing->pipe_lock.Unlock();
    msgr->lock.Unlock();
    goto reply;    

  reply:
    reply.features = ((uint64_t)connect.features & policy.features_supported) | policy.features_required;
    reply.authorizer_len = authorizer_reply.length();
    rc = tcp_write((char*)&reply, sizeof(reply));
    if (rc < 0)
      goto fail_unlocked;
    if (reply.authorizer_len) {
      rc = tcp_write(authorizer_reply.c_str(), authorizer_reply.length());
      if (rc < 0)
	goto fail_unlocked;
    }
  }
  
 replace:
  if (connect.features & CEPH_FEATURE_RECONNECT_SEQ) {
    reply_tag = CEPH_MSGR_TAG_SEQ;
    existing_seq = existing->in_seq;
  }
  ldout(msgr->cct,10) << "accept replacing " << existing << dendl;
  existing->stop();
  existing->unregister_pipe();
  replaced = true;

  if (!existing->policy.lossy) {
    // drop my Connection, and take a ref to the existing one. do not
    // clear existing->connection_state, since read_message and
    // write_message both dereference it without pipe_lock.
    connection_state->put();
    connection_state = existing->connection_state->get();

    // make existing Connection reference us
    existing->connection_state->reset_pipe(this);

    // flush/queue any existing delayed messages
    if (existing->delay_thread)
      existing->delay_thread->flush();

    // steal incoming queue
    uint64_t replaced_conn_id = conn_id;
    conn_id = existing->conn_id;
    existing->conn_id = replaced_conn_id;
    in_seq = existing->in_seq;
    in_seq_acked = in_seq;

    // steal outgoing queue and out_seq
    existing->requeue_sent();
    out_seq = existing->out_seq;
    ldout(msgr->cct,10) << "accept re-queuing on out_seq " << out_seq << " in_seq " << in_seq << dendl;
    for (map<int, list<Message*> >::iterator p = existing->out_q.begin();
         p != existing->out_q.end();
         ++p)
      out_q[p->first].splice(out_q[p->first].begin(), p->second);
  }
  existing->pipe_lock.Unlock();

 open:
  // open
  connect_seq = connect.connect_seq + 1;
  peer_global_seq = connect.global_seq;
  state = STATE_OPEN;
  ldout(msgr->cct,10) << "accept success, connect_seq = " << connect_seq << ", sending READY" << dendl;

  // send READY reply
  reply.tag = (reply_tag ? reply_tag : CEPH_MSGR_TAG_READY);
  reply.features = policy.features_supported;
  reply.global_seq = msgr->get_global_seq();
  reply.connect_seq = connect_seq;
  reply.flags = 0;
  reply.authorizer_len = authorizer_reply.length();
  if (policy.lossy)
    reply.flags = reply.flags | CEPH_MSG_CONNECT_LOSSY;

  connection_state->set_features((uint64_t)reply.features & (uint64_t)connect.features);
  ldout(msgr->cct,10) << "accept features " << connection_state->get_features() << dendl;

  delete session_security;
  session_security = get_auth_session_handler(msgr->cct, connect.authorizer_protocol, session_key,
					      connection_state->get_features());

  // notify
  msgr->dispatch_queue.queue_accept(connection_state);

  // ok!
  if (msgr->dispatch_queue.stop)
    goto shutting_down;
  register_pipe();
  msgr->lock.Unlock();

  rc = tcp_write((char*)&reply, sizeof(reply));
  if (rc < 0) {
    goto fail_registered;
  }

  if (reply.authorizer_len) {
    rc = tcp_write(authorizer_reply.c_str(), authorizer_reply.length());
    if (rc < 0) {
      goto fail_registered;
    }
  }

  if (reply_tag == CEPH_MSGR_TAG_SEQ) {
    if(tcp_write((char*)&existing_seq, sizeof(existing_seq)) < 0) {
      ldout(msgr->cct,2) << "accept write error on in_seq" << dendl;
      goto fail_registered;
    }
    if (tcp_read((char*)&newly_acked_seq, sizeof(newly_acked_seq)) < 0) {
      ldout(msgr->cct,2) << "accept read error on newly_acked_seq" << dendl;
      goto fail_registered;
    }
  }

  pipe_lock.Lock();
  discard_requeued_up_to(newly_acked_seq);
  if (state != STATE_CLOSED) {
    ldout(msgr->cct,10) << "accept starting writer, state " << get_state_name() << dendl;
    start_writer();
  }
  ldout(msgr->cct,20) << "accept done" << dendl;
  pipe_lock.Unlock();

  maybe_start_delay_thread();

  return 0;   // success.

 fail_registered:
  ldout(msgr->cct, 10) << "accept fault after register" << dendl;

  if (msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << " sleep for " << msgr->cct->_conf->ms_inject_internal_delays << dendl;
    utime_t t;
    t.set_from_double(msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

 fail_unlocked:
  pipe_lock.Lock();
  if (state != STATE_CLOSED) {
    bool queued = is_queued();
    ldout(msgr->cct, 10) << "  queued = " << (int)queued << dendl;
    if (queued) {
      state = policy.server ? STATE_STANDBY : STATE_CONNECTING;
    } else if (replaced) {
      state = STATE_STANDBY;
    } else {
      state = STATE_CLOSED;
      state_closed.set(1);
    }
    fault();
    if (queued || replaced)
      start_writer();
  }
  pipe_lock.Unlock();
  return -1;

 shutting_down:
  msgr->lock.Unlock();

  if (msgr->cct->_conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << " sleep for " << msgr->cct->_conf->ms_inject_internal_delays << dendl;
    utime_t t;
    t.set_from_double(msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

  pipe_lock.Lock();
  state = STATE_CLOSED;
  state_closed.set(1);
  fault();
  pipe_lock.Unlock();
  return -1;
}

void Pipe::set_socket_options()
{
  // disable Nagle algorithm?
  if (msgr->cct->_conf->ms_tcp_nodelay) {
    int flag = 1;
    int r = ::rsetsockopt(sd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
    if (r < 0) {
      r = -errno;
      ldout(msgr->cct,0) << "couldn't set TCP_NODELAY: " << cpp_strerror(r) << dendl;
    }
  }
  if (msgr->cct->_conf->ms_tcp_rcvbuf) {
    int size = msgr->cct->_conf->ms_tcp_rcvbuf;
    int r = ::rsetsockopt(sd, SOL_SOCKET, SO_RCVBUF, (void*)&size, sizeof(size));
    if (r < 0)  {
      r = -errno;
      ldout(msgr->cct,0) << "couldn't set SO_RCVBUF to " << size << ": " << cpp_strerror(r) << dendl;
    }
  }
}

int Pipe::connect()
{
  bool got_bad_auth = false;

  ldout(msgr->cct,10) << "connect " << connect_seq << dendl;
  assert(pipe_lock.is_locked());

  __u32 cseq = connect_seq;
  __u32 gseq = msgr->get_global_seq();

  // stop reader thrad
  join_reader();

  pipe_lock.Unlock();
  
  char tag = -1;
  int rc;
  struct msghdr msg;
  struct iovec msgvec[2];
  int msglen;
  char banner[strlen(CEPH_BANNER) + 1];  // extra byte makes coverity happy
  entity_addr_t paddr;
  entity_addr_t peer_addr_for_me, socket_addr;
  AuthAuthorizer *authorizer = NULL;
  bufferlist addrbl, myaddrbl;
  const md_config_t *conf = msgr->cct->_conf;

  // close old socket.  this is safe because we stopped the reader thread above.
  if (sd >= 0)
    ::rclose(sd);

  char buf[80];

  // create socket?
  sd = ::rsocket(peer_addr.get_family(), SOCK_STREAM, 0);
  if (sd < 0) {
    lderr(msgr->cct) << "connect couldn't created socket " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }

  // connect!
  ldout(msgr->cct,10) << "connecting to " << peer_addr << dendl;
  rc = ::rconnect(sd, (sockaddr*)&peer_addr.addr, peer_addr.addr_size());
  if (rc < 0) {
    ldout(msgr->cct,2) << "connect error " << peer_addr
	     << ", " << errno << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }

  set_socket_options();

  // verify banner
  // FIXME: this should be non-blocking, or in some other way verify the banner as we get it.
  if (tcp_read((char*)&banner, strlen(CEPH_BANNER)) < 0) {
    ldout(msgr->cct,2) << "connect couldn't read banner, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }
  if (memcmp(banner, CEPH_BANNER, strlen(CEPH_BANNER))) {
    ldout(msgr->cct,0) << "connect protocol error (bad banner) on peer " << paddr << dendl;
    goto fail;
  }

  memset(&msg, 0, sizeof(msg));
  msgvec[0].iov_base = banner;
  msgvec[0].iov_len = strlen(CEPH_BANNER);
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 1;
  msglen = msgvec[0].iov_len;
  if (do_sendmsg(&msg, msglen)) {
    ldout(msgr->cct,2) << "connect couldn't write my banner, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }

  // identify peer
  {
    bufferptr p(sizeof(paddr) * 2);
    addrbl.push_back(p);
  }
  if (tcp_read(addrbl.c_str(), addrbl.length()) < 0) {
    ldout(msgr->cct,2) << "connect couldn't read peer addrs, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }
  {
    bufferlist::iterator p = addrbl.begin();
    ::decode(paddr, p);
    ::decode(peer_addr_for_me, p);
    port = peer_addr_for_me.get_port();
  }

  ldout(msgr->cct,20) << "connect read peer addr " << paddr << " on socket " << sd << dendl;
  if (peer_addr != paddr) {
    if (paddr.is_blank_ip() &&
	peer_addr.get_port() == paddr.get_port() &&
	peer_addr.get_nonce() == paddr.get_nonce()) {
      ldout(msgr->cct,0) << "connect claims to be " 
	      << paddr << " not " << peer_addr << " - presumably this is the same node!" << dendl;
    } else {
      ldout(msgr->cct,0) << "connect claims to be " 
	      << paddr << " not " << peer_addr << " - wrong node!" << dendl;
      goto fail;
    }
  }

  ldout(msgr->cct,20) << "connect peer addr for me is " << peer_addr_for_me << dendl;

  msgr->learned_addr(peer_addr_for_me);

  ::encode(msgr->my_inst.addr, myaddrbl);

  memset(&msg, 0, sizeof(msg));
  msgvec[0].iov_base = myaddrbl.c_str();
  msgvec[0].iov_len = myaddrbl.length();
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 1;
  msglen = msgvec[0].iov_len;
  if (do_sendmsg(&msg, msglen)) {
    ldout(msgr->cct,2) << "connect couldn't write my addr, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
    goto fail;
  }
  ldout(msgr->cct,10) << "connect sent my addr " << msgr->my_inst.addr << dendl;


  while (1) {
    delete authorizer;
    authorizer = msgr->get_authorizer(peer_type, false);
    bufferlist authorizer_reply;

    ceph_msg_connect connect;
    connect.features = policy.features_supported;
    connect.host_type = msgr->my_type;
    connect.global_seq = gseq;
    connect.connect_seq = cseq;
    connect.protocol_version = msgr->get_proto_version(peer_type, true);
    connect.authorizer_protocol = authorizer ? authorizer->protocol : 0;
    connect.authorizer_len = authorizer ? authorizer->bl.length() : 0;
    if (authorizer) 
      ldout(msgr->cct,10) << "connect.authorizer_len=" << connect.authorizer_len
	       << " protocol=" << connect.authorizer_protocol << dendl;
    connect.flags = 0;
    if (policy.lossy)
      connect.flags |= CEPH_MSG_CONNECT_LOSSY;  // this is fyi, actually, server decides!
    memset(&msg, 0, sizeof(msg));
    msgvec[0].iov_base = (char*)&connect;
    msgvec[0].iov_len = sizeof(connect);
    msg.msg_iov = msgvec;
    msg.msg_iovlen = 1;
    msglen = msgvec[0].iov_len;
    if (authorizer) {
      msgvec[1].iov_base = authorizer->bl.c_str();
      msgvec[1].iov_len = authorizer->bl.length();
      msg.msg_iovlen++;
      msglen += msgvec[1].iov_len;
    }

    ldout(msgr->cct,10) << "connect sending gseq=" << gseq << " cseq=" << cseq
	     << " proto=" << connect.protocol_version << dendl;
    if (do_sendmsg(&msg, msglen)) {
      ldout(msgr->cct,2) << "connect couldn't write gseq, cseq, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      goto fail;
    }

    ldout(msgr->cct,20) << "connect wrote (self +) cseq, waiting for reply" << dendl;
    ceph_msg_connect_reply reply;
    if (tcp_read((char*)&reply, sizeof(reply)) < 0) {
      ldout(msgr->cct,2) << "connect read reply " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      goto fail;
    }
    ldout(msgr->cct,20) << "connect got reply tag " << (int)reply.tag
	     << " connect_seq " << reply.connect_seq
	     << " global_seq " << reply.global_seq
	     << " proto " << reply.protocol_version
	     << " flags " << (int)reply.flags
	     << dendl;

    authorizer_reply.clear();

    if (reply.authorizer_len) {
      ldout(msgr->cct,10) << "reply.authorizer_len=" << reply.authorizer_len << dendl;
      bufferptr bp = buffer::create(reply.authorizer_len);
      if (tcp_read(bp.c_str(), reply.authorizer_len) < 0) {
        ldout(msgr->cct,10) << "connect couldn't read connect authorizer_reply" << dendl;
	goto fail;
      }
      authorizer_reply.push_back(bp);
    }

    if (authorizer) {
      bufferlist::iterator iter = authorizer_reply.begin();
      if (!authorizer->verify_reply(iter)) {
        ldout(msgr->cct,0) << "failed verifying authorize reply" << dendl;
	goto fail;
      }
    }

    if (conf->ms_inject_internal_delays) {
      ldout(msgr->cct, 10) << " sleep for " << msgr->cct->_conf->ms_inject_internal_delays << dendl;
      utime_t t;
      t.set_from_double(msgr->cct->_conf->ms_inject_internal_delays);
      t.sleep();
    }

    pipe_lock.Lock();
    if (state != STATE_CONNECTING) {
      ldout(msgr->cct,0) << "connect got RESETSESSION but no longer connecting" << dendl;
      goto stop_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_FEATURES) {
      ldout(msgr->cct,0) << "connect protocol feature mismatch, my " << std::hex
	      << connect.features << " < peer " << reply.features
	      << " missing " << (reply.features & ~policy.features_supported)
	      << std::dec << dendl;
      goto fail_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_BADPROTOVER) {
      ldout(msgr->cct,0) << "connect protocol version mismatch, my " << connect.protocol_version
	      << " != " << reply.protocol_version << dendl;
      goto fail_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_BADAUTHORIZER) {
      ldout(msgr->cct,0) << "connect got BADAUTHORIZER" << dendl;
      if (got_bad_auth)
        goto stop_locked;
      got_bad_auth = true;
      pipe_lock.Unlock();
      delete authorizer;
      authorizer = msgr->get_authorizer(peer_type, true);  // try harder
      continue;
    }
    if (reply.tag == CEPH_MSGR_TAG_RESETSESSION) {
      ldout(msgr->cct,0) << "connect got RESETSESSION" << dendl;
      was_session_reset();
      cseq = 0;
      pipe_lock.Unlock();
      continue;
    }
    if (reply.tag == CEPH_MSGR_TAG_RETRY_GLOBAL) {
      gseq = msgr->get_global_seq(reply.global_seq);
      ldout(msgr->cct,10) << "connect got RETRY_GLOBAL " << reply.global_seq
	       << " chose new " << gseq << dendl;
      pipe_lock.Unlock();
      continue;
    }
    if (reply.tag == CEPH_MSGR_TAG_RETRY_SESSION) {
      assert(reply.connect_seq > connect_seq);
      ldout(msgr->cct,10) << "connect got RETRY_SESSION " << connect_seq
	       << " -> " << reply.connect_seq << dendl;
      cseq = connect_seq = reply.connect_seq;
      pipe_lock.Unlock();
      continue;
    }

    if (reply.tag == CEPH_MSGR_TAG_WAIT) {
      ldout(msgr->cct,3) << "connect got WAIT (connection race)" << dendl;
      state = STATE_WAIT;
      goto stop_locked;
    }

    if (reply.tag == CEPH_MSGR_TAG_READY ||
        reply.tag == CEPH_MSGR_TAG_SEQ) {
      uint64_t feat_missing = policy.features_required & ~(uint64_t)reply.features;
      if (feat_missing) {
	ldout(msgr->cct,1) << "missing required features " << std::hex << feat_missing << std::dec << dendl;
	goto fail_locked;
      }

      if (reply.tag == CEPH_MSGR_TAG_SEQ) {
        ldout(msgr->cct,10) << "got CEPH_MSGR_TAG_SEQ, reading acked_seq and writing in_seq" << dendl;
        uint64_t newly_acked_seq = 0;
        if (tcp_read((char*)&newly_acked_seq, sizeof(newly_acked_seq)) < 0) {
          ldout(msgr->cct,2) << "connect read error on newly_acked_seq" << dendl;
          goto fail_locked;
        }
        handle_ack(newly_acked_seq);
        if (tcp_write((char*)&in_seq, sizeof(in_seq)) < 0) {
          ldout(msgr->cct,2) << "connect write error on in_seq" << dendl;
          goto fail_locked;
        }
      }

      // hooray!
      peer_global_seq = reply.global_seq;
      policy.lossy = reply.flags & CEPH_MSG_CONNECT_LOSSY;
      state = STATE_OPEN;
      connect_seq = cseq + 1;
      assert(connect_seq == reply.connect_seq);
      backoff = utime_t();
      connection_state->set_features((unsigned)reply.features & (unsigned)connect.features);
      ldout(msgr->cct,10) << "connect success " << connect_seq << ", lossy = " << policy.lossy
	       << ", features " << connection_state->get_features() << dendl;
      

      // If we have an authorizer, get a new AuthSessionHandler to deal with ongoing security of the
      // connection.  PLR

      delete session_security;
      if (authorizer != NULL) {
        session_security = get_auth_session_handler(msgr->cct, authorizer->protocol, authorizer->session_key,
						    connection_state->get_features());
      }  else {
        // We have no authorizer, so we shouldn't be applying security to messages in this pipe.  PLR
	session_security = NULL;
      }

      msgr->dispatch_queue.queue_connect(connection_state);
      
      if (!reader_running) {
	ldout(msgr->cct,20) << "connect starting reader" << dendl;
	start_reader();
      }
      maybe_start_delay_thread();
      delete authorizer;
      return 0;
    }
    
    // protocol error
    ldout(msgr->cct,0) << "connect got bad tag " << (int)tag << dendl;
    goto fail_locked;
  }

 fail:
  if (conf->ms_inject_internal_delays) {
    ldout(msgr->cct, 10) << " sleep for " << msgr->cct->_conf->ms_inject_internal_delays << dendl;
    utime_t t;
    t.set_from_double(msgr->cct->_conf->ms_inject_internal_delays);
    t.sleep();
  }

  pipe_lock.Lock();
 fail_locked:
  if (state == STATE_CONNECTING)
    fault();
  else
    ldout(msgr->cct,3) << "connect fault, but state = " << get_state_name()
		       << " != connecting, stopping" << dendl;

 stop_locked:
  delete authorizer;
  return -1;
}

void Pipe::register_pipe()
{
  ldout(msgr->cct,10) << "register_pipe" << dendl;
  assert(msgr->lock.is_locked());
  Pipe *existing = msgr->_lookup_pipe(peer_addr);
  assert(existing == NULL);
  msgr->rank_pipe[peer_addr] = this;
}

void Pipe::unregister_pipe()
{
  assert(msgr->lock.is_locked());
  hash_map<entity_addr_t,Pipe*>::iterator p = msgr->rank_pipe.find(peer_addr);
  if (p != msgr->rank_pipe.end() && p->second == this) {
    ldout(msgr->cct,10) << "unregister_pipe" << dendl;
    msgr->rank_pipe.erase(p);
  } else {
    ldout(msgr->cct,10) << "unregister_pipe - not registered" << dendl;
  }
}


void Pipe::requeue_sent()
{
  if (sent.empty())
    return;

  list<Message*>& rq = out_q[CEPH_MSG_PRIO_HIGHEST];
  while (!sent.empty()) {
    Message *m = sent.back();
    sent.pop_back();
    ldout(msgr->cct,10) << "requeue_sent " << *m << " for resend seq " << out_seq
			<< " (" << m->get_seq() << ")" << dendl;
    rq.push_front(m);
    out_seq--;
  }
}

void Pipe::discard_requeued_up_to(uint64_t seq)
{
  ldout(msgr->cct, 10) << "discard_requeued_up_to " << seq << dendl;
  list<Message*>& rq = out_q[CEPH_MSG_PRIO_HIGHEST];
  while (!rq.empty()) {
    Message *m = rq.front();
    if (m->get_seq() == 0 || m->get_seq() > seq)
      break;
    ldout(msgr->cct,10) << "discard_requeued_up_to " << *m << " for resend seq " << out_seq
			<< " <= " << seq << ", discarding" << dendl;
    m->put();
    rq.pop_front();
    out_seq++;
  }
}

/*
 * Tears down the Pipe's message queues, and removes them from the DispatchQueue
 * Must hold pipe_lock prior to calling.
 */
void Pipe::discard_out_queue()
{
  ldout(msgr->cct,10) << "discard_queue" << dendl;

  for (list<Message*>::iterator p = sent.begin(); p != sent.end(); ++p) {
    ldout(msgr->cct,20) << "  discard " << *p << dendl;
    (*p)->put();
  }
  sent.clear();
  for (map<int,list<Message*> >::iterator p = out_q.begin(); p != out_q.end(); ++p)
    for (list<Message*>::iterator r = p->second.begin(); r != p->second.end(); ++r) {
      ldout(msgr->cct,20) << "  discard " << *r << dendl;
      (*r)->put();
    }
  out_q.clear();
}

void Pipe::fault(bool onread)
{
  const md_config_t *conf = msgr->cct->_conf;
  assert(pipe_lock.is_locked());
  cond.Signal();

  if (onread && state == STATE_CONNECTING) {
    ldout(msgr->cct,10) << "fault already connecting, reader shutting down" << dendl;
    return;
  }
  
  char buf[80];
  ldout(msgr->cct,2) << "fault " << errno << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;

  if (state == STATE_CLOSED ||
      state == STATE_CLOSING) {
    ldout(msgr->cct,10) << "fault already closed|closing" << dendl;
    return;
  }

  shutdown_socket();

  // lossy channel?
  if (policy.lossy && state != STATE_CONNECTING) {
    ldout(msgr->cct,10) << "fault on lossy channel, failing" << dendl;

    stop();

    // crib locks, blech.  note that Pipe is now STATE_CLOSED and the
    // rank_pipe entry is ignored by others.
    pipe_lock.Unlock();

    if (conf->ms_inject_internal_delays) {
      ldout(msgr->cct, 10) << " sleep for " << msgr->cct->_conf->ms_inject_internal_delays << dendl;
      utime_t t;
      t.set_from_double(msgr->cct->_conf->ms_inject_internal_delays);
      t.sleep();
    }

    msgr->lock.Lock();
    pipe_lock.Lock();
    unregister_pipe();
    msgr->lock.Unlock();

    in_q->discard_queue(conn_id);
    if (delay_thread)
      delay_thread->discard();
    discard_out_queue();

    // disconnect from Connection, and mark it failed.  future messages
    // will be dropped.
    assert(connection_state);
    connection_state->clear_pipe(this);

    msgr->dispatch_queue.queue_reset(connection_state);
    return;
  }

  // queue delayed items immediately
  if (delay_thread)
    delay_thread->flush();

  // requeue sent items
  requeue_sent();

  if (policy.standby && !is_queued()) {
    ldout(msgr->cct,0) << "fault with nothing to send, going to standby" << dendl;
    state = STATE_STANDBY;
    return;
  }

  if (state != STATE_CONNECTING) {
    if (policy.server) {
      ldout(msgr->cct,0) << "fault, server, going to standby" << dendl;
      state = STATE_STANDBY;
    } else {
      ldout(msgr->cct,0) << "fault, initiating reconnect" << dendl;
      connect_seq++;
      state = STATE_CONNECTING;
    }
    backoff = utime_t();
  } else if (backoff == utime_t()) {
    ldout(msgr->cct,0) << "fault" << dendl;
    backoff.set_from_double(conf->ms_initial_backoff);
  } else {
    ldout(msgr->cct,10) << "fault waiting " << backoff << dendl;
    cond.WaitInterval(msgr->cct, pipe_lock, backoff);
    backoff += backoff;
    if (backoff > conf->ms_max_backoff)
      backoff.set_from_double(conf->ms_max_backoff);
    ldout(msgr->cct,10) << "fault done waiting or woke up" << dendl;
  }
}

int Pipe::randomize_out_seq()
{
  if (connection_state->get_features() & CEPH_FEATURE_MSG_AUTH) {
    // Set out_seq to a random value, so CRC won't be predictable.   Don't bother checking seq_error
    // here.  We'll check it on the call.  PLR
    int seq_error = get_random_bytes((char *)&out_seq, sizeof(out_seq));
    out_seq &= SEQ_MASK;
    lsubdout(msgr->cct, ms, 10) << "randomize_out_seq " << out_seq << dendl;
    return seq_error;
  } else {
    // previously, seq #'s always started at 0.
    out_seq = 0;
    return 0;
  }
}

void Pipe::was_session_reset()
{
  assert(pipe_lock.is_locked());

  ldout(msgr->cct,10) << "was_session_reset" << dendl;
  in_q->discard_queue(conn_id);
  if (delay_thread)
    delay_thread->discard();
  discard_out_queue();

  msgr->dispatch_queue.queue_remote_reset(connection_state);

  if (randomize_out_seq()) {
    lsubdout(msgr->cct,ms,15) << "was_session_reset(): Could not get random bytes to set seq number for session reset; set seq number to " << out_seq << dendl;
  }

  in_seq = 0;
  connect_seq = 0;
}

void Pipe::stop()
{
  ldout(msgr->cct,10) << "stop" << dendl;
  assert(pipe_lock.is_locked());
  state = STATE_CLOSED;
  state_closed.set(1);
  cond.Signal();
  shutdown_socket();
}


/* read msgs from socket.
 * also, server.
 */
void Pipe::reader()
{
  if (state == STATE_ACCEPTING) 
    accept();

  pipe_lock.Lock();

  // loop.
  while (state != STATE_CLOSED &&
	 state != STATE_CONNECTING) {
    assert(pipe_lock.is_locked());

    // sleep if (re)connecting
    if (state == STATE_STANDBY) {
      ldout(msgr->cct,20) << "reader sleeping during reconnect|standby" << dendl;
      cond.Wait(pipe_lock);
      continue;
    }

    pipe_lock.Unlock();

    char buf[80];
    char tag = -1;
    ldout(msgr->cct,20) << "reader reading tag..." << dendl;
    if (tcp_read((char*)&tag, 1) < 0) {
      pipe_lock.Lock();
      ldout(msgr->cct,2) << "reader couldn't read tag, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      fault(true);
      continue;
    }

    if (tag == CEPH_MSGR_TAG_KEEPALIVE) {
      ldout(msgr->cct,20) << "reader got KEEPALIVE" << dendl;
      pipe_lock.Lock();
      continue;
    }

    // open ...
    if (tag == CEPH_MSGR_TAG_ACK) {
      ldout(msgr->cct,20) << "reader got ACK" << dendl;
      ceph_le64 seq;
      int rc = tcp_read((char*)&seq, sizeof(seq));
      pipe_lock.Lock();
      if (rc < 0) {
	ldout(msgr->cct,2) << "reader couldn't read ack seq, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	fault(true);
      } else if (state != STATE_CLOSED) {
        handle_ack(seq);
      }
      continue;
    }

    else if (tag == CEPH_MSGR_TAG_MSG) {
      ldout(msgr->cct,20) << "reader got MSG" << dendl;
      Message *m = 0;
      int r = read_message(&m);

      pipe_lock.Lock();
      
      if (!m) {
	if (r < 0)
	  fault(true);
	continue;
      }

      if (state == STATE_CLOSED ||
	  state == STATE_CONNECTING) {
	msgr->dispatch_throttle_release(m->get_dispatch_throttle_size());
	m->put();
	continue;
      }

      // check received seq#.  if it is old, drop the message.  
      // note that incoming messages may skip ahead.  this is convenient for the client
      // side queueing because messages can't be renumbered, but the (kernel) client will
      // occasionally pull a message out of the sent queue to send elsewhere.  in that case
      // it doesn't matter if we "got" it or not.
      if (m->get_seq() <= in_seq) {
	ldout(msgr->cct,0) << "reader got old message "
		<< m->get_seq() << " <= " << in_seq << " " << m << " " << *m
		<< ", discarding" << dendl;
	msgr->dispatch_throttle_release(m->get_dispatch_throttle_size());
	m->put();
	continue;
      }

      m->set_connection(connection_state->get());

      // note last received message.
      in_seq = m->get_seq();

      cond.Signal();  // wake up writer, to ack this
      
      ldout(msgr->cct,10) << "reader got message "
	       << m->get_seq() << " " << m << " " << *m
	       << dendl;

      if (delay_thread) {
	utime_t release;
	if (rand() % 10000 < msgr->cct->_conf->ms_inject_delay_probability * 10000.0) {
	  release = m->get_recv_stamp();
	  release += msgr->cct->_conf->ms_inject_delay_max * (double)(rand() % 10000) / 10000.0;
	  lsubdout(msgr->cct, ms, 1) << "queue_received will delay until " << release << " on " << m << " " << *m << dendl;
	}
	delay_thread->queue(release, m);
      } else {
	in_q->enqueue(m, m->get_priority(), conn_id);
      }
    } 
    
    else if (tag == CEPH_MSGR_TAG_CLOSE) {
      ldout(msgr->cct,20) << "reader got CLOSE" << dendl;
      pipe_lock.Lock();
      if (state == STATE_CLOSING) {
	state = STATE_CLOSED;
	state_closed.set(1);
      } else {
	state = STATE_CLOSING;
      }
      cond.Signal();
      break;
    }
    else {
      ldout(msgr->cct,0) << "reader bad tag " << (int)tag << dendl;
      pipe_lock.Lock();
      fault(true);
    }
  }

 
  // reap?
  reader_running = false;
  reader_needs_join = true;
  unlock_maybe_reap();
  ldout(msgr->cct,10) << "reader done" << dendl;
}

/* write msgs to socket.
 * also, client.
 */
void Pipe::writer()
{
  char buf[80];

  pipe_lock.Lock();
  while (state != STATE_CLOSED) {// && state != STATE_WAIT) {
    ldout(msgr->cct,10) << "writer: state = " << get_state_name()
			<< " policy.server=" << policy.server << dendl;

    // standby?
    if (is_queued() && state == STATE_STANDBY && !policy.server) {
      connect_seq++;
      state = STATE_CONNECTING;
    }

    // connect?
    if (state == STATE_CONNECTING) {
      assert(!policy.server);
      connect();
      continue;
    }
    
    if (state == STATE_CLOSING) {
      // write close tag
      ldout(msgr->cct,20) << "writer writing CLOSE tag" << dendl;
      char tag = CEPH_MSGR_TAG_CLOSE;
      state = STATE_CLOSED;
      state_closed.set(1);
      pipe_lock.Unlock();
      if (sd) {
	int r = ::rwrite(sd, &tag, 1);
	// we can ignore r, actually; we don't care if this succeeds.
	r++; r = 0; // placate gcc
      }
      pipe_lock.Lock();
      continue;
    }

    if (state != STATE_CONNECTING && state != STATE_WAIT && state != STATE_STANDBY &&
	(is_queued() || in_seq > in_seq_acked)) {

      // keepalive?
      if (keepalive) {
	pipe_lock.Unlock();
	int rc = write_keepalive();
	pipe_lock.Lock();
	if (rc < 0) {
	  ldout(msgr->cct,2) << "writer couldn't write keepalive, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	  fault();
 	  continue;
	}
	keepalive = false;
      }

      // send ack?
      if (in_seq > in_seq_acked) {
	uint64_t send_seq = in_seq;
	pipe_lock.Unlock();
	int rc = write_ack(send_seq);
	pipe_lock.Lock();
	if (rc < 0) {
	  ldout(msgr->cct,2) << "writer couldn't write ack, " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	  fault();
 	  continue;
	}
	in_seq_acked = send_seq;
      }

      // grab outgoing message
      Message *m = _get_next_outgoing();
      if (m) {
	m->set_seq(++out_seq);
	if (!policy.lossy || close_on_empty) {
	  // put on sent list
	  sent.push_back(m); 
	  m->get();
	}

	// associate message with Connection (for benefit of encode_payload)
	m->set_connection(connection_state->get());

        ldout(msgr->cct,20) << "writer encoding " << m->get_seq() << " " << m << " " << *m << dendl;

	// encode and copy out of *m
	m->encode(connection_state->get_features(), !msgr->cct->_conf->ms_nocrc);

	// prepare everything
	ceph_msg_header& header = m->get_header();
	ceph_msg_footer& footer = m->get_footer();

	// Now that we have all the crcs calculated, handle the
	// digital signature for the message, if the pipe has session
	// security set up.  Some session security options do not
	// actually calculate and check the signature, but they should
	// handle the calls to sign_message and check_signature.  PLR
	if (session_security == NULL) {
	  ldout(msgr->cct, 20) << "writer no session security" << dendl;
	} else {
	  if (session_security->sign_message(m)) {
	    ldout(msgr->cct, 20) << "writer failed to sign seq # " << header.seq
				 << "): sig = " << footer.sig << dendl;
	  } else {
	    ldout(msgr->cct, 20) << "writer signed seq # " << header.seq
				 << "): sig = " << footer.sig << dendl;
	  }
	}

	bufferlist blist = m->get_payload();
	blist.append(m->get_middle());
	blist.append(m->get_data());

	pipe_lock.Unlock();

        ldout(msgr->cct,20) << "writer sending " << m->get_seq() << " " << m << dendl;
	int rc = write_message(header, footer, blist);

	pipe_lock.Lock();
	if (rc < 0) {
          ldout(msgr->cct,1) << "writer error sending " << m << ", "
		  << errno << ": " << strerror_r(errno, buf, sizeof(buf)) << dendl;
	  fault();
        }
	m->put();
      }
      continue;
    }
    
    if (sent.empty() && close_on_empty) {
      ldout(msgr->cct,10) << "writer out and sent queues empty, closing" << dendl;
      stop();
      continue;
    }

    // wait
    ldout(msgr->cct,20) << "writer sleeping" << dendl;
    cond.Wait(pipe_lock);
  }
  
  ldout(msgr->cct,20) << "writer finishing" << dendl;

  // reap?
  writer_running = false;
  unlock_maybe_reap();
  ldout(msgr->cct,10) << "writer done" << dendl;
}

void Pipe::unlock_maybe_reap()
{
  if (!reader_running && !writer_running) {
    shutdown_socket();
    pipe_lock.Unlock();
    msgr->queue_reap(this);
  } else {
    pipe_lock.Unlock();
  }
}

static void alloc_aligned_buffer(bufferlist& data, unsigned len, unsigned off)
{
  // create a buffer to read into that matches the data alignment
  unsigned left = len;
  if (off & ~CEPH_PAGE_MASK) {
    // head
    unsigned head = 0;
    head = MIN(CEPH_PAGE_SIZE - (off & ~CEPH_PAGE_MASK), left);
    bufferptr bp = buffer::create(head);
    data.push_back(bp);
    left -= head;
  }
  unsigned middle = left & CEPH_PAGE_MASK;
  if (middle > 0) {
    bufferptr bp = buffer::create_page_aligned(middle);
    data.push_back(bp);
    left -= middle;
  }
  if (left) {
    bufferptr bp = buffer::create(left);
    data.push_back(bp);
  }
}

int Pipe::read_message(Message **pm)
{
  int ret = -1;
  // envelope
  //ldout(msgr->cct,10) << "receiver.read_message from sd " << sd  << dendl;
  
  ceph_msg_header header; 
  ceph_msg_footer footer;
  __u32 header_crc;
  
  if (connection_state->has_feature(CEPH_FEATURE_NOSRCADDR)) {
    if (tcp_read((char*)&header, sizeof(header)) < 0)
      return -1;
    header_crc = ceph_crc32c_le(0, (unsigned char *)&header, sizeof(header) - sizeof(header.crc));
  } else {
    ceph_msg_header_old oldheader;
    if (tcp_read((char*)&oldheader, sizeof(oldheader)) < 0)
      return -1;
    // this is fugly
    memcpy(&header, &oldheader, sizeof(header));
    header.src = oldheader.src.name;
    header.reserved = oldheader.reserved;
    header.crc = oldheader.crc;
    header_crc = ceph_crc32c_le(0, (unsigned char *)&oldheader, sizeof(oldheader) - sizeof(oldheader.crc));
  }

  ldout(msgr->cct,20) << "reader got envelope type=" << header.type
           << " src " << entity_name_t(header.src)
           << " front=" << header.front_len
	   << " data=" << header.data_len
	   << " off " << header.data_off
           << dendl;

  // verify header crc
  if (header_crc != header.crc) {
    ldout(msgr->cct,0) << "reader got bad header crc " << header_crc << " != " << header.crc << dendl;
    return -1;
  }

  bufferlist front, middle, data;
  int front_len, middle_len;
  unsigned data_len, data_off;
  int aborted;
  Message *message;
  utime_t recv_stamp = ceph_clock_now(msgr->cct);

  if (policy.throttler_messages) {
    ldout(msgr->cct,10) << "reader wants " << 1 << " message from policy throttler "
			<< policy.throttler_messages->get_current() << "/"
			<< policy.throttler_messages->get_max() << dendl;
    policy.throttler_messages->get();
  }

  uint64_t message_size = header.front_len + header.middle_len + header.data_len;
  if (message_size) {
    if (policy.throttler_bytes) {
      ldout(msgr->cct,10) << "reader wants " << message_size << " bytes from policy throttler "
	       << policy.throttler_bytes->get_current() << "/"
	       << policy.throttler_bytes->get_max() << dendl;
      policy.throttler_bytes->get(message_size);
    }

    // throttle total bytes waiting for dispatch.  do this _after_ the
    // policy throttle, as this one does not deadlock (unless dispatch
    // blocks indefinitely, which it shouldn't).  in contrast, the
    // policy throttle carries for the lifetime of the message.
    ldout(msgr->cct,10) << "reader wants " << message_size << " from dispatch throttler "
	     << msgr->dispatch_throttler.get_current() << "/"
	     << msgr->dispatch_throttler.get_max() << dendl;
    msgr->dispatch_throttler.get(message_size);
  }

  utime_t throttle_stamp = ceph_clock_now(msgr->cct);

  // read front
  front_len = header.front_len;
  if (front_len) {
    bufferptr bp = buffer::create(front_len);
    if (tcp_read(bp.c_str(), front_len) < 0)
      goto out_dethrottle;
    front.push_back(bp);
    ldout(msgr->cct,20) << "reader got front " << front.length() << dendl;
  }

  // read middle
  middle_len = header.middle_len;
  if (middle_len) {
    bufferptr bp = buffer::create(middle_len);
    if (tcp_read(bp.c_str(), middle_len) < 0)
      goto out_dethrottle;
    middle.push_back(bp);
    ldout(msgr->cct,20) << "reader got middle " << middle.length() << dendl;
  }


  // read data
  data_len = le32_to_cpu(header.data_len);
  data_off = le32_to_cpu(header.data_off);
  if (data_len) {
    unsigned offset = 0;
    unsigned left = data_len;

    bufferlist newbuf, rxbuf;
    bufferlist::iterator blp;
    int rxbuf_version = 0;
	
    while (left > 0) {
      // wait for data
      if (tcp_read_wait() < 0)
	goto out_dethrottle;

      // get a buffer
      connection_state->lock.Lock();
      map<tid_t,pair<bufferlist,int> >::iterator p = connection_state->rx_buffers.find(header.tid);
      if (p != connection_state->rx_buffers.end()) {
	if (rxbuf.length() == 0 || p->second.second != rxbuf_version) {
	  ldout(msgr->cct,10) << "reader seleting rx buffer v " << p->second.second
		   << " at offset " << offset
		   << " len " << p->second.first.length() << dendl;
	  rxbuf = p->second.first;
	  rxbuf_version = p->second.second;
	  // make sure it's big enough
	  if (rxbuf.length() < data_len)
	    rxbuf.push_back(buffer::create(data_len - rxbuf.length()));
	  blp = p->second.first.begin();
	  blp.advance(offset);
	}
      } else {
	if (!newbuf.length()) {
	  ldout(msgr->cct,20) << "reader allocating new rx buffer at offset " << offset << dendl;
	  alloc_aligned_buffer(newbuf, data_len, data_off);
	  blp = newbuf.begin();
	  blp.advance(offset);
	}
      }
      bufferptr bp = blp.get_current_ptr();
      int read = MIN(bp.length(), left);
      ldout(msgr->cct,20) << "reader reading nonblocking into " << (void*)bp.c_str() << " len " << bp.length() << dendl;
      int got = tcp_read_nonblocking(bp.c_str(), read);
      ldout(msgr->cct,30) << "reader read " << got << " of " << read << dendl;
      connection_state->lock.Unlock();
      if (got < 0)
	goto out_dethrottle;
      if (got > 0) {
	blp.advance(got);
	data.append(bp, 0, got);
	offset += got;
	left -= got;
      } // else we got a signal or something; just loop.
    }
  }

  // footer
  if (connection_state->has_feature(CEPH_FEATURE_MSG_AUTH)) {
    if (tcp_read((char*)&footer, sizeof(footer)) < 0)
      goto out_dethrottle;
  } else {
    ceph_msg_footer_old old_footer;
    if (tcp_read((char*)&old_footer, sizeof(old_footer)) < 0)
      goto out_dethrottle;
    footer.front_crc = old_footer.front_crc;
    footer.middle_crc = old_footer.middle_crc;
    footer.data_crc = old_footer.data_crc;
    footer.sig = 0;
    footer.flags = old_footer.flags;
  }
  
  aborted = (footer.flags & CEPH_MSG_FOOTER_COMPLETE) == 0;
  ldout(msgr->cct,10) << "aborted = " << aborted << dendl;
  if (aborted) {
    ldout(msgr->cct,0) << "reader got " << front.length() << " + " << middle.length() << " + " << data.length()
	    << " byte message.. ABORTED" << dendl;
    ret = 0;
    goto out_dethrottle;
  }

  ldout(msgr->cct,20) << "reader got " << front.length() << " + " << middle.length() << " + " << data.length()
	   << " byte message" << dendl;
  message = decode_message(msgr->cct, header, footer, front, middle, data);
  if (!message) {
    ret = -EINVAL;
    goto out_dethrottle;
  }

  //
  //  Check the signature if one should be present.  A zero return indicates success. PLR
  //

  if (session_security == NULL) {
    ldout(msgr->cct, 10) << "No session security set" << dendl;
  } else {
    if (session_security->check_message_signature(message)) {
      ldout(msgr->cct, 0) << "Signature check failed" << dendl;
      ret = -EINVAL;
      goto out_dethrottle;
    } 
  }

  message->set_byte_throttler(policy.throttler_bytes);
  message->set_message_throttler(policy.throttler_messages);

  // store reservation size in message, so we don't get confused
  // by messages entering the dispatch queue through other paths.
  message->set_dispatch_throttle_size(message_size);

  message->set_recv_stamp(recv_stamp);
  message->set_throttle_stamp(throttle_stamp);
  message->set_recv_complete_stamp(ceph_clock_now(msgr->cct));

  *pm = message;
  return 0;

 out_dethrottle:
  // release bytes reserved from the throttlers on failure
  if (policy.throttler_messages) {
    ldout(msgr->cct,10) << "reader releasing " << 1 << " message to policy throttler "
			<< policy.throttler_messages->get_current() << "/"
			<< policy.throttler_messages->get_max() << dendl;
    policy.throttler_messages->put();
  }
  if (message_size) {
    if (policy.throttler_bytes) {
      ldout(msgr->cct,10) << "reader releasing " << message_size << " bytes to policy throttler "
			  << policy.throttler_bytes->get_current() << "/"
			  << policy.throttler_bytes->get_max() << dendl;
      policy.throttler_bytes->put(message_size);
    }

    msgr->dispatch_throttle_release(message_size);
  }
  return ret;
}

int Pipe::do_sendmsg(struct msghdr *msg, int len, bool more)
{
  char buf[80];

  while (len > 0) {
    if (0) { // sanity
      int l = 0;
      for (unsigned i=0; i<msg->msg_iovlen; i++)
	l += msg->msg_iov[i].iov_len;
      assert(l == len);
    }

    int r = ::rsendmsg(sd, msg, MSG_NOSIGNAL | (more ? MSG_MORE : 0));
    if (r == 0) 
      ldout(msgr->cct,10) << "do_sendmsg hmm do_sendmsg got r==0!" << dendl;
    if (r < 0) { 
      ldout(msgr->cct,1) << "do_sendmsg error " << strerror_r(errno, buf, sizeof(buf)) << dendl;
      return -1;
    }
    if (state == STATE_CLOSED) {
      ldout(msgr->cct,10) << "do_sendmsg oh look, state == CLOSED, giving up" << dendl;
      errno = EINTR;
      return -1; // close enough
    }

    len -= r;
    if (len == 0) break;
    
    // hrmph.  trim r bytes off the front of our message.
    ldout(msgr->cct,20) << "do_sendmsg short write did " << r << ", still have " << len << dendl;
    while (r > 0) {
      if (msg->msg_iov[0].iov_len <= (size_t)r) {
	// lose this whole item
	//ldout(msgr->cct,30) << "skipping " << msg->msg_iov[0].iov_len << ", " << (msg->msg_iovlen-1) << " v, " << r << " left" << dendl;
	r -= msg->msg_iov[0].iov_len;
	msg->msg_iov++;
	msg->msg_iovlen--;
      } else {
	// partial!
	//ldout(msgr->cct,30) << "adjusting " << msg->msg_iov[0].iov_len << ", " << msg->msg_iovlen << " v, " << r << " left" << dendl;
	msg->msg_iov[0].iov_base = (char *)msg->msg_iov[0].iov_base + r;
	msg->msg_iov[0].iov_len -= r;
	break;
      }
    }
  }
  return 0;
}


int Pipe::write_ack(uint64_t seq)
{
  ldout(msgr->cct,10) << "write_ack " << seq << dendl;

  char c = CEPH_MSGR_TAG_ACK;
  ceph_le64 s;
  s = seq;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec msgvec[2];
  msgvec[0].iov_base = &c;
  msgvec[0].iov_len = 1;
  msgvec[1].iov_base = &s;
  msgvec[1].iov_len = sizeof(s);
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 2;
  
  if (do_sendmsg(&msg, 1 + sizeof(s), true) < 0)
    return -1;	
  return 0;
}

int Pipe::write_keepalive()
{
  ldout(msgr->cct,10) << "write_keepalive" << dendl;

  char c = CEPH_MSGR_TAG_KEEPALIVE;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec msgvec[2];
  msgvec[0].iov_base = &c;
  msgvec[0].iov_len = 1;
  msg.msg_iov = msgvec;
  msg.msg_iovlen = 1;
  
  if (do_sendmsg(&msg, 1) < 0)
    return -1;	
  return 0;
}


int Pipe::write_message(ceph_msg_header& header, ceph_msg_footer& footer, bufferlist& blist)
{
  int ret;

  // set up msghdr and iovecs
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  struct iovec *msgvec = new iovec[3 + blist.buffers().size()];  // conservative upper bound
  msg.msg_iov = msgvec;
  int msglen = 0;
  
  // send tag
  char tag = CEPH_MSGR_TAG_MSG;
  msgvec[msg.msg_iovlen].iov_base = &tag;
  msgvec[msg.msg_iovlen].iov_len = 1;
  msglen++;
  msg.msg_iovlen++;

  // send envelope
  ceph_msg_header_old oldheader;
  if (connection_state->has_feature(CEPH_FEATURE_NOSRCADDR)) {
    msgvec[msg.msg_iovlen].iov_base = (char*)&header;
    msgvec[msg.msg_iovlen].iov_len = sizeof(header);
    msglen += sizeof(header);
    msg.msg_iovlen++;
  } else {
    memcpy(&oldheader, &header, sizeof(header));
    oldheader.src.name = header.src;
    oldheader.src.addr = connection_state->get_peer_addr();
    oldheader.orig_src = oldheader.src;
    oldheader.reserved = header.reserved;
    oldheader.crc = ceph_crc32c_le(0, (unsigned char*)&oldheader,
			      sizeof(oldheader) - sizeof(oldheader.crc));
    msgvec[msg.msg_iovlen].iov_base = (char*)&oldheader;
    msgvec[msg.msg_iovlen].iov_len = sizeof(oldheader);
    msglen += sizeof(oldheader);
    msg.msg_iovlen++;
  }

  // payload (front+data)
  list<bufferptr>::const_iterator pb = blist.buffers().begin();
  int b_off = 0;  // carry-over buffer offset, if any
  int bl_pos = 0; // blist pos
  int left = blist.length();

  while (left > 0) {
    int donow = MIN(left, (int)pb->length()-b_off);
    if (donow == 0) {
      ldout(msgr->cct,0) << "donow = " << donow << " left " << left << " pb->length " << pb->length()
	      << " b_off " << b_off << dendl;
    }
    assert(donow > 0);
    ldout(msgr->cct,30) << " bl_pos " << bl_pos << " b_off " << b_off
	     << " leftinchunk " << left
	     << " buffer len " << pb->length()
	     << " writing " << donow 
	     << dendl;
    
    if (msg.msg_iovlen >= IOV_MAX-2) {
      if (do_sendmsg(&msg, msglen, true))
	goto fail;
      
      // and restart the iov
      msg.msg_iov = msgvec;
      msg.msg_iovlen = 0;
      msglen = 0;
    }
    
    msgvec[msg.msg_iovlen].iov_base = (void*)(pb->c_str()+b_off);
    msgvec[msg.msg_iovlen].iov_len = donow;
    msglen += donow;
    msg.msg_iovlen++;
    
    left -= donow;
    assert(left >= 0);
    b_off += donow;
    bl_pos += donow;
    if (left == 0)
      break;
    while (b_off == (int)pb->length()) {
      ++pb;
      b_off = 0;
    }
  }
  assert(left == 0);

  // send footer; if receiver doesn't support signatures, use the old footer format

  ceph_msg_footer_old old_footer;
  if (connection_state->has_feature(CEPH_FEATURE_MSG_AUTH)) {
    msgvec[msg.msg_iovlen].iov_base = (void*)&footer;
    msgvec[msg.msg_iovlen].iov_len = sizeof(footer);
    msglen += sizeof(footer);
    msg.msg_iovlen++;
  } else {
    old_footer.front_crc = footer.front_crc;   
    old_footer.middle_crc = footer.middle_crc;   
    old_footer.data_crc = footer.data_crc;   
    old_footer.flags = footer.flags;   
    msgvec[msg.msg_iovlen].iov_base = (char*)&old_footer;
    msgvec[msg.msg_iovlen].iov_len = sizeof(old_footer);
    msglen += sizeof(old_footer);
    msg.msg_iovlen++;
  }

  // send
  if (do_sendmsg(&msg, msglen))
    goto fail;

  ret = 0;

 out:
  delete[] msgvec;
  return ret;

 fail:
  ret = -1;
  goto out;
}


int Pipe::tcp_read(char *buf, int len)
{
  if (sd < 0)
    return -1;

  while (len > 0) {

    if (msgr->cct->_conf->ms_inject_socket_failures && sd >= 0) {
      if (rand() % msgr->cct->_conf->ms_inject_socket_failures == 0) {
	ldout(msgr->cct, 0) << "injecting socket failure" << dendl;
	::rshutdown(sd, SHUT_RDWR);
      }
    }

    if (tcp_read_wait() < 0)
      return -1;

    int got = tcp_read_nonblocking(buf, len);

    if (got < 0)
      return -1;

    len -= got;
    buf += got;
    //lgeneric_dout(cct, DBL) << "tcp_read got " << got << ", " << len << " left" << dendl;
  }
  return len;
}

int Pipe::tcp_read_wait()
{
  if (sd < 0)
    return -1;
  struct pollfd pfd;
  short evmask;
  pfd.fd = sd;
  pfd.events = POLLIN;
#if defined(__linux__)
  pfd.events |= POLLRDHUP;
#endif

  if (rpoll(&pfd, 1, msgr->timeout) <= 0)
    return -1;

  evmask = POLLERR | POLLHUP | POLLNVAL;
#if defined(__linux__)
  evmask |= POLLRDHUP;
#endif
  if (pfd.revents & evmask)
    return -1;

  if (!(pfd.revents & POLLIN))
    return -1;

  return 0;
}

int Pipe::tcp_read_nonblocking(char *buf, int len)
{
again:
  int got = ::rrecv( sd, buf, len, MSG_DONTWAIT );
  if (got < 0) {
    if (errno == EAGAIN || errno == EINTR) {
      goto again;
    } else {
      ldout(msgr->cct, 10) << "tcp_read_nonblocking socket " << sd << " returned "
		     << got << " errno " << errno << " " << cpp_strerror(errno) << dendl;
      return -1;
    }
  } else if (got == 0) {
    /* poll() said there was data, but we didn't read any - peer
     * sent a FIN.  Maybe POLLRDHUP signals this, but this is
     * standard socket behavior as documented by Stevens.
     */
    return -1;
  }
  return got;
}

int Pipe::tcp_write(const char *buf, int len)
{
  if (sd < 0)
    return -1;
  struct pollfd pfd;
  pfd.fd = sd;
  pfd.events = POLLOUT | POLLHUP | POLLNVAL | POLLERR;
#if defined(__linux__)
  pfd.events |= POLLRDHUP;
#endif

  if (msgr->cct->_conf->ms_inject_socket_failures && sd >= 0) {
    if (rand() % msgr->cct->_conf->ms_inject_socket_failures == 0) {
      ldout(msgr->cct, 0) << "injecting socket failure" << dendl;
      ::rshutdown(sd, SHUT_RDWR);
    }
  }

  if (rpoll(&pfd, 1, -1) < 0)
    return -1;

  if (!(pfd.revents & POLLOUT))
    return -1;

  //lgeneric_dout(cct, DBL) << "tcp_write writing " << len << dendl;
  assert(len > 0);
  while (len > 0) {
    int did = ::rsend( sd, buf, len, MSG_NOSIGNAL );
    if (did < 0) {
      //lgeneric_dout(cct, 1) << "tcp_write error did = " << did << "  errno " << errno << " " << strerror(errno) << dendl;
      //lgeneric_derr(cct, 1) << "tcp_write error did = " << did << "  errno " << errno << " " << strerror(errno) << dendl;
      return did;
    }
    len -= did;
    buf += did;
    //lgeneric_dout(cct, DBL) << "tcp_write did " << did << ", " << len << " left" << dendl;
  }
  return 0;
}
