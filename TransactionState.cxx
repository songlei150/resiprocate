#include <sipstack/TransactionState.hxx>
#include <sipstack/SipStack.hxx>
#include <sipstack/SipMessage.hxx>
#include <sipstack/TimerMessage.hxx>
#include <sipstack/MethodTypes.hxx>
#include <sipstack/Helper.hxx>
#include <sipstack/SendingMessage.hxx>
#include <util/Logger.hxx>

using namespace Vocal2;

#define VOCAL_SUBSYSTEM Subsystem::SIP

TransactionState::TransactionState(SipStack& stack, Machine m, State s) : 
   mStack(stack),
   mMachine(m), 
   mState(s),
   mIsReliable(false), // !jf! 
   mCancelStateMachine(0),
   mMsgToRetransmit(0)
{
}

TransactionState::~TransactionState()
{
   const Data& tid = mMsgToRetransmit->getTransactionId();
   mStack.mTransactionMap.remove(tid);

   delete mCancelStateMachine;
   mCancelStateMachine = 0;
   
   delete mMsgToRetransmit;
   mMsgToRetransmit = 0;

   mState = Bogus;
}


void
TransactionState::process(SipStack& stack)
{
   Message* message = stack.mStateMacFifo.getNext();
   assert(message);
   DebugLog (<< "got message out of state machine fifo: " << *message);
   
   SipMessage* sip = dynamic_cast<SipMessage*>(message);
   TimerMessage* timer=0;
   
   if (sip == 0)
   {
      timer = dynamic_cast<TimerMessage*>(message);
   }
   else if (!sip->isExternal() && 
            sip->isRequest() && 
            sip->header(h_RequestLine).getMethod() == ACK) 
   {
      // for ACK messages from the TU, there is no transaction, send it directly
      // to the wire // rfc3261 17.1 Client Transaction
      stack.mTransportSelector.send(sip);
   }
   

   const Data& tid = message->getTransactionId();
   TransactionState* state = stack.mTransactionMap.find(tid);
   if (state) // found transaction for sip msg
   {
      DebugLog (<< "Found transaction for msg " << *state);
      
      switch (state->mMachine)
      {
         case ClientNonInvite:
            state->processClientNonInvite(message);
            break;
         case ClientInvite:
            state->processClientInvite(message);
            break;
         case ServerNonInvite:
            state->processServerNonInvite(message);
            break;
         case ServerInvite:
            state->processServerInvite(message);
            break;
         case Stale:
            state->processStale(message);
            break;
         default:
            assert(0);
      }
   }
   else // new transaction
   {
      if (sip)
      {
         DebugLog (<< "Create new transaction for sip msg ");

         if (sip->isRequest())
         {
            // create a new state object and insert in the TransactionMap
               
            if (sip->isExternal()) // new sip msg from transport
            {
               DebugLog (<< "Create new transaction for inbound msg ");
               if (sip->header(h_RequestLine).getMethod() == INVITE)
               {
                   DebugLog(<<" adding T100 timer (INV)");
                  TransactionState* state = new TransactionState(stack, ServerInvite, Proceeding);
		  // !rk! This might be needlessly created.  Design issue.
		  state->mMsgToRetransmit = state->make100(sip);
                  stack.mTimers.add(Timer::TimerTrying, tid, Timer::T100);
                  stack.mTransactionMap.add(tid,state);
               }
               else 
               {
                   DebugLog(<<"Adding non-INVITE transaction state");
                  TransactionState* state = new TransactionState(stack, ServerNonInvite,Trying);
                  stack.mTransactionMap.add(tid,state);
               }
               DebugLog(<< "Adding incoming message to TU fifo");
               stack.mTUFifo.add(sip);
            }
            else // new sip msg from the TU
            {
               DebugLog (<< "Create new transaction for msg from TU ");
               if (sip->header(h_RequestLine).getMethod() == INVITE)
               {
                  TransactionState* state = new TransactionState(stack, ClientInvite, Calling);
                  stack.mTransactionMap.add(tid,state);
                  state->processClientInvite(sip);
               }
               else 
               {
                  TransactionState* state = new TransactionState(stack, ClientNonInvite, Trying);
                  stack.mTransactionMap.add(tid,state);
                  state->processClientNonInvite(sip);
               }
            }
         }
         else if (sip->isResponse()) // stray response
         {
            if (stack.mDiscardStrayResponses)
            {
               DebugLog (<< "discarding stray response: " << sip->brief());
               delete message;
            }
            else
            {
               // forward this statelessly
                DebugLog(<<"forward this statelessly -- UNIMP");
               assert(0);
            }
         }
         else // wasn't a request or a response
         {
            DebugLog (<< "discarding unknown message: " << sip->brief());
	 }
      } 
      else // timer or other non-sip msg
      {
         DebugLog (<< "discarding non-sip message: " << message->brief());
         delete message;
      }
   }
}

void
TransactionState::processClientNonInvite(  Message* msg )
{ 
   if (isRequest(msg) && !isInvite(msg) && isFromTU(msg))
   {
      DebugLog (<< "received new non-invite request");
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      mMsgToRetransmit = sip;
      mStack.mTimers.add(Timer::TimerF, msg->getTransactionId(), 64*Timer::T1 );
      sendToWire(sip);  // don't delete
   }
   else if (isSentReliable(msg))
   {
      DebugLog (<< "received sent reliably message");
      // ignore
      delete msg;
   } 
   else if (isSentUnreliable(msg))
   {
      DebugLog (<< "received sent unreliably message");
      // state might affect this !jf!
      // should we set mIsReliable = false here !jf!
      mStack.mTimers.add(Timer::TimerE1, msg->getTransactionId(), Timer::T1 );
      delete msg;
   }
   else if (isResponse(msg) && !isFromTU(msg)) // from the wire
   {
      DebugLog (<< "received response from wire");

      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      int code = sip->header(h_StatusLine).responseCode();
      if (code >= 100 && code < 200) // 1XX
      {
         if (mState == Trying || mState == Proceeding)
         {
            mState = Proceeding;
            if (!mIsReliable)
            {
               mStack.mTimers.add(Timer::TimerE2, msg->getTransactionId(), Timer::T2 );
            }
            sendToTU(msg); // don't delete            
         }
         else
         {
            // ignore
            delete msg;
         }
      }
      else if (code >= 200)
      {
         if (mIsReliable)
         {
            sendToTU(msg); // don't delete
            delete this;
         }
         else
         {
            mState = Completed;
            mStack.mTimers.add(Timer::TimerK, msg->getTransactionId(), Timer::T4 );            
            sendToTU(msg); // don't delete            
         }
      }
   }
   else if (isTimer(msg))
   {
      DebugLog (<< "received timer in client non-invite transaction");

      TimerMessage* timer = dynamic_cast<TimerMessage*>(msg);
      switch (timer->getType())
      {
         case Timer::TimerE1:
            if (mState == Trying)
            {
               unsigned long d = timer->getDuration();
               if (d < Timer::T2) d *= 2;
               mStack.mTimers.add(Timer::TimerE1, msg->getTransactionId(), d);
               mStack.mTransportSelector.retransmit(mMsgToRetransmit); 
               delete msg;
            }
            else
            {
               // ignore
               delete msg;
            }
            break;

         case Timer::TimerE2:
            if (mState == Proceeding)
            {
               mStack.mTimers.add(Timer::TimerE2, msg->getTransactionId(), Timer::T2);
               mStack.mTransportSelector.retransmit(mMsgToRetransmit); 
               delete msg;
            }
            else 
            {
               // ignore
               delete msg;
            }
            break;

         case Timer::TimerF:
            // !jf! is this correct
            sendToTU(Helper::makeResponse(*mMsgToRetransmit, 408));
            delete this;
            break;

         case Timer::TimerK:
            delete this;
            break;

         default:

            assert(0);
            break;
      }
   }
   else if (isTranportError(msg))
   {
      // inform the TU
      assert(0);
      delete this;
   }
}


void
TransactionState::processClientInvite(  Message* msg )
{
   DebugLog(<< "TransactionState::processClientInvite: " << *msg);
   
   if (isInvite(msg) && isFromTU(msg))
   {
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      switch (sip->header(h_RequestLine).getMethod())
      {
         case INVITE:
            mMsgToRetransmit = sip;
            mStack.mTimers.add(Timer::TimerB, msg->getTransactionId(), 64*Timer::T1 );
            sendToWire(msg); // don't delete msg
            break;
            
         case CANCEL:
            mCancelStateMachine = new TransactionState(mStack, ClientNonInvite, Trying);
            mStack.mTransactionMap.add(msg->getTransactionId(), mCancelStateMachine);
            mCancelStateMachine->processClientNonInvite(msg);
            sendToWire(msg); // don't delete msg
            break;
            
         default:
            delete msg;
            break;
      }
   }
   else if (isSentIndication(msg))
   {
      switch (mMsgToRetransmit->header(h_RequestLine).getMethod())
      {
         case INVITE:
            if (isSentReliable(msg))
            {
               mStack.mTimers.add(Timer::TimerA, msg->getTransactionId(), Timer::T1 );
            }
            delete msg;
            break;
            
         case CANCEL:
            mCancelStateMachine->processClientNonInvite(msg);
            // !jf! memory mgmt? 
            break;
            
         default:
            delete msg;
            break;
      }
   }
   else if (isResponse(msg) && !isFromTU(msg))
   {
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      int code = sip->header(h_StatusLine).responseCode();
      switch (sip->header(h_CSeq).method())
      {
         case INVITE:
            if (code >= 100 && code < 200) // 1XX
            {
               if (mState == Calling || mState == Proceeding)
               {
                  mState = Proceeding;
                  sendToTU(sip); // don't delete msg
               }
               else
               {
                  delete msg;
               }
            }
            else if (code >= 200 && code < 300)
            {
               mMachine = Stale;
               mState = Terminated;
               mStack.mTimers.add(Timer::TimerStale, msg->getTransactionId(), Timer::TS );               
               sendToTU(sip); // don't delete msg               
            }
            else if (code >= 300)
            {
               if (mIsReliable)
               {
                  SipMessage* invite = mMsgToRetransmit;
                  mMsgToRetransmit = Helper::makeFailureAck(*invite, *sip);
                  delete invite;
                  
                  // want to use the same transport as was selected for Invite
                  mStack.mTransportSelector.retransmit(mMsgToRetransmit);  
                  sendToTU(msg); // don't delete msg
                  delete this;
               }
               else
               {
                  if (mState == Calling || mState == Proceeding)
                  {
                     mState = Completed;
                     SipMessage* invite = mMsgToRetransmit;
                     mStack.mTimers.add(Timer::TimerD, msg->getTransactionId(), Timer::TD );
                     mMsgToRetransmit = Helper::makeFailureAck(*invite, *sip);
                     delete invite;
                     mStack.mTransportSelector.retransmit(mMsgToRetransmit);  
                     sendToTU(msg); // don't delete msg
                  }
                  else if (mState == Completed)
                  {
                     mStack.mTransportSelector.retransmit(mMsgToRetransmit);  
                     sendToTU(msg); // don't delete msg
                  }
                  else
                  {
                     assert(0);
                  }
               }
            }
            break;
            
         case CANCEL:
            mCancelStateMachine->processClientNonInvite(msg);
            // !jf! memory mgmt? 
            break;

         default:
            delete msg;
            break;
      }
   }
   else if (isTimer(msg))
   {
      TimerMessage* timer = dynamic_cast<TimerMessage*>(msg);
      DebugLog (<< "timer fired: " << *timer);
      
      switch (timer->getType())
      {
         case Timer::TimerA:
            if (mState == Calling)
            {
               unsigned long d = timer->getDuration();
               if (d < Timer::T2) d *= 2;

               mStack.mTimers.add(Timer::TimerA, msg->getTransactionId(), d);
               mStack.mTransportSelector.retransmit(mMsgToRetransmit);  
            }
            delete msg;
            break;

         case Timer::TimerB:
            // inform TU 
            delete msg;
            delete this;
            assert(0);
            break;

         case Timer::TimerD:
            delete msg;
            delete this;
            break;

         default:
            assert(mCancelStateMachine);
            mCancelStateMachine->processClientNonInvite(msg);
            break;
      }
   }
   else if (isTranportError(msg))
   {
      // inform TU 
      delete msg;
      delete this;
      assert(0);
   }
}


void
TransactionState::processServerNonInvite(  Message* msg )
{
   if (isRequest(msg) && !isInvite(msg) && !isFromTU(msg)) // from the wire
   {
      if (mState == Trying)
      {
         // ignore
         delete msg;
      }
      else if (mState == Proceeding || mState == Trying)
      {
         mStack.mTransportSelector.retransmit(mMsgToRetransmit);  
         delete msg;
      }
      else
      {
         assert(0);
      }
   }
   else if (isResponse(msg) && isFromTU(msg))
   {
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      int code = sip->header(h_StatusLine).responseCode();
      if (code >= 100 && code < 200) // 1XX
      {
         if (mState == Trying || mState == Proceeding)
         {
            delete mMsgToRetransmit;
            mMsgToRetransmit = sip;
            mState = Proceeding;
            sendToWire(sip); // don't delete msg
         }
         else
         {
            // ignore
            delete msg;
         }
      }
      else if (code >= 200 && code <= 699)
      {
         if (mIsReliable)
         {
            mMsgToRetransmit = sip;
            sendToWire(sip); // don't delete msg
            delete this;
         }
         else
         {
            if (mState == Trying || mState == Proceeding)
            {
               mState = Completed;
               mStack.mTimers.add(Timer::TimerJ, msg->getTransactionId(), 64*Timer::T1 );
               mMsgToRetransmit = sip;
               sendToWire(sip); // don't delete msg
            }
            else if (mState == Completed)
            {
               // ignore
               delete msg;               
            }
            else
            {
               assert(0);
            }
         }
      }
      else
      {
         // ignore
         delete msg;               
      }
   }
   else if (isTimer(msg))
   {
      assert (mState == Completed);
      assert(dynamic_cast<TimerMessage*>(msg)->getType() == Timer::TimerJ);
      delete msg;
      delete this;
   }
   else if (isTranportError(msg))
   {
      // inform TU 
      delete msg;
      delete this;
      assert(0);
   }
}


void
TransactionState::processServerInvite(  Message* msg )
{
   if (isRequest(msg) && !isFromTU(msg))
   {
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      switch (sip->header(h_RequestLine).getMethod())
      {
         case INVITE:
            if (mState == Proceeding || mState == Completed)
            {
               DebugLog (<< "Received invite from wire - forwarding to TU state=" << mState);
	       if (!mMsgToRetransmit)
	       {
	          mMsgToRetransmit = make100(sip); // for when TimerTrying fires
               }
	       sendToTU(msg); // don't delete
            }
            else
            {
               DebugLog (<< "Received invite from wire - ignoring state=" << mState);
               delete msg;
            }
            break;
            
         case ACK:
            if (mState == Completed)
            {
               if (mIsReliable)
               {
                  DebugLog (<< "Received ACK in Completed (reliable) - delete transaction");
                  delete this; 
                  delete msg;
               }
               else
               {
                  DebugLog (<< "Received ACK in Completed (unreliable) - confirmed, start Timer I");
                  mState = Confirmed;
                  mStack.mTimers.add(Timer::TimerI, msg->getTransactionId(), Timer::T4 );
                  delete msg;
               }
            }
            else
            {
               DebugLog (<< "Ignore ACK not in Completed state");
               delete msg;
            }
            break;

         case CANCEL:
            DebugLog (<< "Received Cancel, create Cancel transaction and process as server non-invite and send to TU");
            mCancelStateMachine = new TransactionState(mStack, ServerNonInvite, Trying);
            mStack.mTransactionMap.add(msg->getTransactionId(), mCancelStateMachine);
            mCancelStateMachine->processServerNonInvite(msg);
            sendToTU(msg); // don't delete msg
            break;

         default:
            DebugLog (<< "Received unexpected request. Ignoring message");
            delete msg;
            break;
      }
   }
   else if (isResponse(msg, 100, 699) && isFromTU(msg))
   {
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      int code = sip->header(h_StatusLine).responseCode();
      switch (sip->header(h_CSeq).method())
      {
         case INVITE:
            if (code == 100)
            {
               if (mState == Trying)               
               {
                  DebugLog (<< "Received 100 in Trying State. Send over wire");
                  delete mMsgToRetransmit; // may be replacing the 100
                  mMsgToRetransmit = sip;
                  mState = Proceeding;
                  sendToWire(msg); // don't delete msg
               }
               else
               {
                  DebugLog (<< "Received 100 when not in Trying State. Ignoring");
                  delete msg;
               }
            }
            else if (code > 100 && code < 200)
            {
               if (mState == Trying || mState == Proceeding)
               {
                  DebugLog (<< "Received 100 in Trying or Proceeding. Send over wire");
                  delete mMsgToRetransmit; // may be replacing the 100
                  mMsgToRetransmit = sip;
                  mState = Proceeding;
                  sendToWire(msg); // don't delete msg
               }
               else
               {
                  DebugLog (<< "Received 100 when not in Trying State. Ignoring");
                  delete msg;
               }
            }
            else if (code >= 200 && code < 300)
            {
               if (mState == Trying || mState == Proceeding)
               {
                  DebugLog (<< "Received 2xx when in Trying or Proceeding State. Start Stale Timer, move to terminated.");
                  delete mMsgToRetransmit; 
                  mMsgToRetransmit = sip; // save it, even though it won't be transmitted
                  mMachine = Stale;
                  mState = Terminated;
                  mStack.mTimers.add(Timer::TimerStale, msg->getTransactionId(), Timer::TS );
                  sendToWire(msg); // don't delete
               }
               else
               {
                  DebugLog (<< "Received 2xx when not in Trying or Proceeding State. Ignoring");
                  delete msg;
               }
            }
            else if (code >= 300)
            {
               if (mState == Trying || mState == Proceeding)
               {
                  DebugLog (<< "Received failed response in Trying or Proceeding. Start Timer H, move to completed.");
                  delete mMsgToRetransmit; 
                  mMsgToRetransmit = sip; // save it, even though it won't be transmitted
                  mMachine = Stale;
                  mState = Completed;
                  mStack.mTimers.add(Timer::TimerH, msg->getTransactionId(), 64*Timer::T1 );
                  if (!mIsReliable)
                  {
                     mStack.mTimers.add(Timer::TimerG, msg->getTransactionId(), Timer::T1 );
                  }
                  sendToWire(msg); // don't delete msg
               }
               else
               {
                  DebugLog (<< "Received Final response when not in Trying or Proceeding State. Ignoring");
                  delete msg;
               }
            }
            else
            {
               DebugLog (<< "Received Invalid response line. Ignoring");
               delete msg;
            }
            break;
            
         case CANCEL:
            DebugLog (<< "Received Cancel, create Cancel transaction and process as server non-invite and send to TU");
            
            mCancelStateMachine = new TransactionState(mStack, ServerNonInvite, Trying);
            mStack.mTransactionMap.add(msg->getTransactionId(), mCancelStateMachine);
            mCancelStateMachine->processServerNonInvite(msg);
            sendToTU(msg); // don't delete
            break;
            
         default:
            DebugLog (<< "Received response to non invite or cancel. Ignoring");
            delete msg;
            break;
      }
   }
   else if (isTimer(msg))
   {
      TimerMessage* timer = dynamic_cast<TimerMessage*>(msg);
      switch (timer->getType())
      {
         case Timer::TimerG:
            if (mState == Completed)
            {
               DebugLog (<< "TimerG fired. retransmit, and readd TimerG");
               mStack.mTransportSelector.retransmit(mMsgToRetransmit);  
               mStack.mTimers.add(Timer::TimerG, msg->getTransactionId(), timer->getDuration()*2 );
            }
            else
            {
               delete msg;
            }
            break;
            
         case Timer::TimerH:
         case Timer::TimerI:
            DebugLog (<< "TimerH or TimerI fired. Delete this");
            delete this;
            delete msg;
            break;
            
         case Timer::TimerJ:
            DebugLog (<< "TimerJ fired. Delete state of cancel");
            mCancelStateMachine = 0;
            delete mCancelStateMachine;
            delete msg;
            break;
            
         case Timer::TimerTrying:
            if (mState == Proceeding)
            {
               DebugLog (<< "TimerTrying fired. Send a 100");
               sendToWire(mMsgToRetransmit); // will get deleted when this is deleted
               delete msg;
            }
            else
            {
               DebugLog (<< "TimerTrying fired. Not in Proceeding state. Ignoring");
               delete msg;
            }
            break;
            
         default:
            assert(0); // programming error if any other timer fires
            break;
      }
   }
   else if (isTranportError(msg))
   {
      DebugLog (<< "Transport error received. Delete this");
      delete this;
      delete msg;
   }
   else
   {
      DebugLog (<< "TransactionState::processServerInvite received " << *msg << " out of context"); 
      delete msg; // !jf!
   }
}


void
TransactionState::processStale(  Message* msg )
{
}

bool
TransactionState::isRequest(Message* msg) const
{
   SipMessage* sip = dynamic_cast<SipMessage*>(msg);   
   return sip && sip->isRequest();
}

bool
TransactionState::isInvite(Message* msg) const
{
   if (isRequest(msg))
   {
      SipMessage* sip = dynamic_cast<SipMessage*>(msg);
      return (sip->header(h_RequestLine).getMethod()) == INVITE;
   }
   return false;
}

bool
TransactionState::isResponse(Message* msg, int lower, int upper) const
{
   SipMessage* sip = dynamic_cast<SipMessage*>(msg);
   if (sip && sip->isResponse())
   {
      int c = sip->header(h_StatusLine).responseCode();
      return (c >= lower && c <= upper);
   }
   return false;
}

bool
TransactionState::isTimer(Message* msg) const
{
   return dynamic_cast<TimerMessage*>(msg) != 0;
}


bool
TransactionState::isFromTU(Message* msg) const
{
   SipMessage* sip = dynamic_cast<SipMessage*>(msg);
   return sip && !sip->isExternal();
}

bool
TransactionState::isTranportError(Message* msg) const
{
   return false; // !jf!
}

bool
TransactionState::isSentIndication(Message* msg) const
{
   return msg && dynamic_cast<SendingMessage*>(msg);
}

bool
TransactionState::isSentReliable(Message* msg) const
{
   SendingMessage* sending = dynamic_cast<SendingMessage*>(msg);
   return (sending && sending->isReliable());
}

bool
TransactionState::isSentUnreliable(Message* msg) const
{
   SendingMessage* sending = dynamic_cast<SendingMessage*>(msg);
   return (sending && !sending->isReliable());
}


void
TransactionState::sendToWire(Message* msg) const
{
   SipMessage* sip=dynamic_cast<SipMessage*>(msg);
   assert(sip);
   mStack.mTransportSelector.send(sip);
}

void
TransactionState::sendToTU(Message* msg) const
{
   SipMessage* sip=dynamic_cast<SipMessage*>(msg);
   assert(sip);
   mStack.mTUFifo.add(sip);
}

SipMessage*
TransactionState::make100(SipMessage* request) const
{
   SipMessage* sip=Helper::makeResponse(*request, 100);
   return sip;
}


std::ostream& 
Vocal2::operator<<(std::ostream& strm, const Vocal2::TransactionState& state)
{
   strm << "Tstate[ mMach=" << state.mMachine 
        <<  " mState="  << state.mState 
        << " mIsRel=" << state.mIsReliable;
   return strm;
}


/* ====================================================================
 * The Vovida Software License, Version 1.0 
 * 
 * Copyright (c) 2000 Vovida Networks, Inc.  All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 
 * 3. The names "VOCAL", "Vovida Open Communication Application Library",
 *    and "Vovida Open Communication Application Library (VOCAL)" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact vocal@vovida.org.
 *
 * 4. Products derived from this software may not be called "VOCAL", nor
 *    may "VOCAL" appear in their name, without prior written
 *    permission of Vovida Networks, Inc.
 * 
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL VOVIDA
 * NETWORKS, INC. OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT DAMAGES
 * IN EXCESS OF $1,000, NOR FOR ANY INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * 
 * ====================================================================
 * 
 * This software consists of voluntary contributions made by Vovida
 * Networks, Inc. and many individuals on behalf of Vovida Networks,
 * Inc.  For more information on Vovida Networks, Inc., please see
 * <http://www.vovida.org/>.
 *
 */
