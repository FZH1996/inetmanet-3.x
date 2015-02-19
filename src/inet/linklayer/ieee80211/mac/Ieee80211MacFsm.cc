
#include "inet/linklayer/ieee80211/mac/Ieee80211Mac.h"
#include "inet/linklayer/ieee80211/mac/Ieee80211MpduA.h"


namespace inet {

namespace ieee80211 {

// TODO: handle MPDU-A of a frame like normal transmission

/*
bool Ieee80211Mac::MpduAKey::operator<(const MpduAKey& other) const
{
    if (other.addr != addr)
        return other.addr < addr;
    return other.seq < seq;
}

bool Ieee80211Mac::MpduAKey::operator==(const MpduAKey& other) const
{
    return (addr == other.addr && seq == other.seq);
}

bool Ieee80211Mac::MpduAKey::operator!=(const MpduAKey& other) const
{
    return (addr != other.addr || seq != other.seq);
}
*/

void Ieee80211Mac::retryCurrentMpduA()
{
    ASSERT(retryCounter() < transmissionLimit - 1);
    getCurrentTransmission()->setRetry(true);
    if (rateControlMode == RATE_AARF || rateControlMode == RATE_ARF)
        reportDataFailed();
    else
        retryMpduA++;

    numRetry()++;
    backoff() = true;
    generateBackoffPeriod();
}

void Ieee80211Mac::setBlockAckTimeOut()
{
    uint64_t blkAckFrameSize = 152*8;
    uint64_t blocAckReqSize = 24*8;
    // TODO: check this time out
    simtime_t delay = computeFrameDuration(blocAckReqSize, bitrate) + SIMTIME_DBL( getSlotTime()) +SIMTIME_DBL( getSIFS()) + computeFrameDuration(blkAckFrameSize, bitrate) + MAX_PROPAGATION_DELAY * 2;
    scheduleAt(simTime()+delay,endTimeout);
}

// Handle MPDU-A frames.
void Ieee80211Mac::sendMpuAPending(const MACAddress &addr)
{
    auto it = processingMpdu.find(addr);
    if (it == processingMpdu.end())
        return;
    // Send to up until first erroneous
    while (!it->second.inReception.empty() && !it->second.inReception.front()->hasBitError())
    {
        // send to up
        if (!it->second.inReception.front()->hasBitError())
            sendUp(it->second.inReception.front());
        else
            delete it->second.inReception.front();
        it->second.inReception.pop_front();
    }
    processingMpdu.erase(it);
}

// Handle MPDU-A frames.
void Ieee80211Mac::processMpduA(Ieee80211DataOrMgmtFrame *frame)
{

    if (!isForUs(frame))
        return;
    // if is correct store the frame in the confirmation array

    bool duplicate = isDuplicated(frame);

    if (frame->hasBitError())
        numCollision++;

    auto it = processingMpdu.find(frame->getTransmitterAddress());
    if (it == processingMpdu.end())
    {
        // clear old
        for (auto itAux = processingMpdu.begin();itAux != processingMpdu.end();)
        {
            if (simTime() - itAux->second.time > 2)
                processingMpdu.erase(itAux++);
            else
                ++itAux;
        }
        MpduAInProc proc;
        proc.time = simTime();
        processingMpdu[frame->getTransmitterAddress()] = proc;
        it  = processingMpdu.find(frame->getTransmitterAddress());
        // check old frames
    }
    else
    {
        //
        if (!MpduModeReception)
        {
            // check if the node has received the seqNumber before
            bool included = false;
            if (!duplicate)
            {
                for (unsigned int i = 0; i < it->second.inReception.size(); i++)
                {
                    if (it->second.inReception[i]->getSequenceNumber() == frame->getSequenceNumber())
                    {
                        included = true;
                        break;
                    }
                }
            }
            if (!duplicate && !included) // other frame clean stored information
            {
                if (simTime() - it->second.time < 0.001)
                { // before delete send up received frames
                    for (unsigned int i = 0; i < it->second.inReception.size(); i++)
                    {
                        if (!it->second.inReception[i]->hasBitError())
                            sendUp(it->second.inReception[i]);
                    }
                }
                while (!it->second.inReception.empty())
                {
                    delete it->second.inReception.front();
                    it->second.inReception.pop_front();
                }
                it->second.time = simTime();
            }
        }
    }

    if (!MpduModeReception)
    {
        MpduModeReception = true;
        it->second.confirmed.clear();
    }

    if (it->second.timeOut.isScheduled()) // cancel time out
        cancelEvent(&(it->second.timeOut));

    if (frame->getLastMpdu())
        MpduModeReception = false;

    if (!frame->hasBitError())
        it->second.confirmed.push_back(frame->getSequenceNumber());

    if (!duplicate)
    { // if the frame has been sent to upper doesn't include it in pending list
        bool included = false;
        for (auto &elem : it->second.inReception)
        {
            if (elem->getSequenceNumber() == frame->getSequenceNumber())
            {
                // check errors
                if (elem->hasBitError() && !frame->hasBitError())
                {
                    // actualize
                    elem->setBitError(frame->hasBitError());
                }
                included = true;
                break;
            }
        }
        if (!included)
            it->second.inReception.push_back(frame->dup()); // include in the pending list.
    }

    if (frame->getLastMpdu())
    {
        // Send to up until first erroneous
        while (!it->second.inReception.empty() && !it->second.inReception.front()->hasBitError())
        {
            // send to up
            sendUp(it->second.inReception.front());
            it->second.inReception.pop_front();
        }
        // prepare timer if there are pending correct frames.
        if (!it->second.inReception.empty())
        {
            // only set the timer if there is frames correctly received pending of to send to the upper layers
            bool setTimer = false;
            for (auto &elem : it->second.inReception)
            {
                if (!elem->hasBitError())
                {
                    setTimer = true;
                    break;
                }
            }
            if (setTimer)
            {
                // there are correct frames in the pending list, set the time out
                it->second.timeOut.setControlInfo(&it->second);
                scheduleAt(simTime()+mpudRetTimeOut,&(it->second.timeOut));
            }
        }
    }
}

bool Ieee80211Mac::isMpduA(Ieee80211Frame *frame)
{
    if (dynamic_cast<Ieee80211MpduA*>(frame))
        return true;
    return false;
}

void Ieee80211Mac::sendBLOCKACKFrameOnEndSIFS()
{
    Ieee80211Frame *frameToACK = (Ieee80211Frame *)endSIFS->getContextPointer();
    Ieee80211DataOrMgmtFrame *frame = dynamic_cast<Ieee80211DataOrMgmtFrame *>(frameToACK);
    MACAddress addr = frame->getTransmitterAddress();
    endSIFS->setContextPointer(nullptr);
    delete frameToACK;
    sendBlockAckFrame(addr);
}


void Ieee80211Mac::sendBlockAckFrame(const MACAddress & addr)
{
    auto it = processingMpdu.find(addr);
    if (it == processingMpdu.end())
        throw cRuntimeError("No data for to send Block Ack frame  %s",addr.str().c_str());

    Ieee80211BlockAckFrame * frame = new Ieee80211BlockAckFrame();
    if (!it->second.confirmed.empty())
    {
        frame->setStartingSequence(it->second.confirmed.front());
        frame->setSequencesArraySize(it->second.confirmed.size()-1);
    }

    for (int i = 0 ; i < ((int)it->second.confirmed.size()-1);i++)
        frame->setSequences(i,it->second.confirmed[i+1]);

    frame->setDuration(0);
    frame->setReceiverAddress(addr);
    frame->setTransmitterAddress(address);
    sendDown(setBitrateFrame(frame));
}

void Ieee80211Mac::processBlockAckFrame(Ieee80211Frame *frameToSend)
{
    MpduAInReception retFrames;
    std::vector<int> retries;

    Ieee80211BlockAckFrame *frame  = check_and_cast<Ieee80211BlockAckFrame*>(frameToSend);

    // process first packet correctly received.

    while (mpduInTransmission->getNumEncap()>0 && frame->getStartingSequence() != mpduInTransmission->getPacket(0)->getSequenceNumber())
    {
        int retry = mpduInTransmission->getNumRetries(0);
        if (retry == transmissionLimit - 1)
        {
            delete mpduInTransmission->decapsulatePacket(0); // packet lost.
            numGivenUp()++;
            continue;
        }
        retries.push_back(retry+1);
        retFrames.push_back(mpduInTransmission->decapsulatePacket(0));
    }

    if (mpduInTransmission->getNumEncap()>0)
    {
        Ieee80211DataOrMgmtFrame *pkt = mpduInTransmission->decapsulatePacket(0);
        numBits += pkt->getBitLength();
        bits() += pkt->getBitLength();
        macDelay()->record(simTime() - pkt->getMACArrive());
        if (maxJitter() == SIMTIME_ZERO || maxJitter() < (simTime() - pkt->getMACArrive()))
            maxJitter() = simTime() - fr->getMACArrive();
        if (minJitter() == SIMTIME_ZERO || minJitter() > (simTime() - pkt->getMACArrive()))
            minJitter() = simTime() - pkt->getMACArrive();
        if (fsm->debug()) EV_DEBUG << "record macDelay AC" << currentAC << " value " << simTime() - pkt->getMACArrive() <<endl;


        int retry = mpduInTransmission->getNumRetries(0);
        if (retry == 0) numSentWithoutRetry()++;
        numSent()++;
        delete pkt; // packet correctly received.
    }

    // check the other packets.

    for (int i = 0 ; i < (int)frame->getSequencesArraySize();i++)
    {
        while (mpduInTransmission->getNumEncap()>0 && frame->getSequences(i) != mpduInTransmission->getPacket(0)->getSequenceNumber())
        {
            int retry = mpduInTransmission->getNumRetries(0);
            if (retry == transmissionLimit - 1)
            {
                delete mpduInTransmission->decapsulatePacket(0); // packet lost.
                numGivenUp()++;
                continue;
            }
            retries.push_back(retry+1);
            retFrames.push_back(mpduInTransmission->decapsulatePacket(0));
        }
        if (mpduInTransmission->getNumEncap()>0)
        {
            int retry = mpduInTransmission->getNumRetries(0);
            if (retry == 0) numSentWithoutRetry()++;
            numSent()++;
            Ieee80211DataOrMgmtFrame *pkt = mpduInTransmission->decapsulatePacket(0);
            numBits += pkt->getBitLength();
            bits() += pkt->getBitLength();
            macDelay()->record(simTime() - pkt->getMACArrive());
            if (maxJitter() == SIMTIME_ZERO || maxJitter() < (simTime() - pkt->getMACArrive()))
                maxJitter() = simTime() - fr->getMACArrive();
            if (minJitter() == SIMTIME_ZERO || minJitter() > (simTime() - pkt->getMACArrive()))
                minJitter() = simTime() - pkt->getMACArrive();
            if (fsm->debug()) EV_DEBUG << "record macDelay AC" << currentAC << " value " << simTime() - pkt->getMACArrive() <<endl;
            delete pkt; // packet correctly received.
        }
    }
    if (retFrames.empty())
    {
        // transmission success
        mpduInTransmission = nullptr;
        finishCurrentTransmission();
    }
    else
    {
        // retransmissions
        for (unsigned int i = 0; i < retFrames.size(); i++)
        {
            mpduInTransmission->pushBack(retFrames[i],retries[i]);
        }
        if (mpduInTransmission->getNumEncap()<64 && numConsecutiveMpduA < maxConsecutiveMpduA)
        {
            retryMpduA++;
            numConsecutiveMpduA++;
            // request packet and create a new MTUA with size 64
            cMessage *msg = queueModule->requestMpuA(mpduInTransmission->getReceiverAddress(), 64 - mpduInTransmission->getNumEncap(),mpduInTransmission->getByteLength(), mpduAClass);
            if (msg != nullptr)
            {
                take(msg);
                Ieee80211MpduA  *mpdua = check_and_cast<Ieee80211MpduA*>(msg);
                while (mpdua->getNumEncap() > 0)
                {
                    Ieee80211DataOrMgmtFrame * frame = mpdua->popFront();
                    frame->setMACArrive(simTime());
                    mpduInTransmission->pushBack(frame);
                }
            }
            numMduRequested++;
        }
    }
}


Ieee80211MpduDelimiter *Ieee80211Mac::buildMpduDataFrame(Ieee80211Frame *frameToSend, int retry)
{
    Ieee80211DataOrMgmtFrame *frameAux = dynamic_cast<Ieee80211DataOrMgmtFrame *>(frameToSend);
    if (frameAux && retry  == 0)
    {
        frameAux->setSequenceNumber(sequenceNumber);
        sequenceNumber = (sequenceNumber+1) % 4096;  //XXX seqNum must be checked upon reception of frames!
    }

    Ieee80211Frame *frame = (Ieee80211DataOrMgmtFrame *)frameToSend->dup();

    if (frameAux && frameAux->getTransmitterAddress().isUnspecified())
    {
        frameAux->setTransmitterAddress(address);
    }
    if (frameToSend->getControlInfo() != nullptr)
    {
        cObject * ctr = frameToSend->getControlInfo();
        TransmissionRequest *ctrl = dynamic_cast <TransmissionRequest*> (ctr);
        if (ctrl == nullptr)
            throw cRuntimeError("control info is not PhyControlInfo type %s");
        frame->setControlInfo(ctrl->dup());
    }
    frame->setDuration(getSIFS() + controlFrameTxTime(LENGTH_ACK));
    Ieee80211MpduDelimiter * delimiter = new Ieee80211MpduDelimiter();
    delimiter->encapsulate(frame);
    while ((delimiter->getByteLength()%4) != 0)
        delimiter->setByteLength(delimiter->getByteLength()+1);
    return delimiter;
}

bool Ieee80211Mac::initFsm(cMessage *msg,bool &receptionError, Ieee80211Frame *& frame)
{
    removeOldTuplesFromDuplicateMap();
    // skip those cases where there's nothing to do, so the switch looks simpler
    if (isUpperMessage(msg) && fsm->getState() != IDLE) {
        if (fsm->getState() == WAITAIFS && endDIFS->isScheduled()) {
            // a difs was schedule because all queues ware empty
            // change difs for aifs
            simtime_t remaint = getAIFS(currentAC) - getDIFS();
            scheduleAt(endDIFS->getArrivalTime() + remaint, endAIFS(currentAC));
            cancelEvent(endDIFS);
        }
        else if (fsm->getState() == BACKOFF && endBackoff(numCategories() - 1)->isScheduled() && transmissionQueue(numCategories() - 1)->empty()) {
            // a backoff was schedule with all the queues empty
            // reschedule the backoff with the appropriate AC
            backoffPeriod(currentAC) = backoffPeriod(numCategories() - 1);
            backoff(currentAC) = backoff(numCategories() - 1);
            backoff(numCategories() - 1) = false;
            scheduleAt(endBackoff(numCategories() - 1)->getArrivalTime(), endBackoff(currentAC));
            cancelEvent(endBackoff(numCategories() - 1));
        }
        if (fsm->debug()) EV_DEBUG << "deferring upper message transmission in " << fsm->getStateName() << " state\n";
        return false;
    }

    if (msg->isSelfMessage())
    {
        MpduAInProc *proc = dynamic_cast<MpduAInProc *>(msg->getControlInfo());
        if (proc)
        {
            // end time out, send to up the rest of the frame
            auto it = processingMpdu.begin();
            for (;it!=processingMpdu.end();++it)
            {
                if (&it->second == proc)
                    break;
            }
            if (it == processingMpdu.end())
                throw cRuntimeError("MpduAInProc not found");
            while (!it->second.inReception.empty())
            {
                // send to up
                if (!it->second.inReception.front()->hasBitError())
                    sendUp(it->second.inReception.front());
                it->second.inReception.pop_front();
            }
            processingMpdu.erase(it);
            return false;
        }
    }

// Special case, is  endTimeout ACK and the radio state  is RECV, the system must wait until end reception (9.3.2.8 ACK procedure)
    if (msg == endTimeout && isMediumRecv() && useModulationParameters && fsm->getState() == WAITACK)
    {
        EV << "Re-schedule WAITACK timeout \n";
        scheduleAt(simTime() + controlFrameTxTime(LENGTH_ACK), endTimeout);
        return false;
    }

    logState();
    stateVector.record(fsm->getState());

    receptionError = false;
    frame = dynamic_cast<Ieee80211Frame*>(msg);
    if (frame && isLowerMessage(frame)) {
        lastReceiveFailed = receptionError = frame ? frame->hasBitError() : false;
        scheduleReservePeriod(frame);
    }
    return true;
}

void Ieee80211Mac::endFsm(cMessage *msg)
{
    EV_TRACE << "leaving handleWithFSM\n\t";
    logState();
    stateVector.record(fsm->getState());
    if (simTime() - last > 0.1)
    {
        for (int i = 0; i<numCategories(); i++)
        {
            throughput(i)->record(bits(i)/(simTime()-last));
            bits(i) = 0;
            if (maxJitter(i) > SIMTIME_ZERO && minJitter(i) > SIMTIME_ZERO)
            {
                jitter(i)->record(maxJitter(i)-minJitter(i));
                maxJitter(i) = SIMTIME_ZERO;
                minJitter(i) = SIMTIME_ZERO;
            }
        }
        last = simTime();
    }
}

void Ieee80211Mac::stateIdle(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        sendDownPendingRadioConfigMsg();
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isUpperMessage(msg))
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Data - Ready \n";
        ASSERT(isInvalidBackoffPeriod() || backoffPeriod() == SIMTIME_ZERO);
        invalidateBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
    FSMIEEE80211_No_Event_Transition(fsmLocal,!transmissionQueueEmpty())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Data - Ready \n";
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
    FSMIEEE80211_Event_Transition(fsmLocal,isLowerMessage(msg))
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive \n";
        FSMIEEE80211_Transition(fsmLocal,RECEIVE);
    }
}


void Ieee80211Mac::stateDefer(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        sendDownPendingRadioConfigMsg();
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isMediumStateChange(msg) && isMediumFree())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Wait - AIFS \n";
        FSMIEEE80211_Transition(fsmLocal,WAITAIFS);
    }
    FSMIEEE80211_No_Event_Transition(fsmLocal, isMediumFree() || (!isBackoffPending()))
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Wait - AIFS \n";
        FSMIEEE80211_Transition(fsmLocal,WAITAIFS);
    }
    FSMIEEE80211_Event_Transition(fsmLocal,isLowerMessage(msg))
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive \n";
        FSMIEEE80211_Transition(fsmLocal,RECEIVE);
    }
}


void Ieee80211Mac::stateWaitAifs(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        scheduleAIFSPeriod();
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isMsgAIFS(msg) && transmissionQueue()->empty())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "EDCAF - Do - Nothing \n";
        ASSERT(0 == 1);
        FSMIEEE80211_Transition(fsmLocal,WAITAIFS);
    }

    FSMIEEE80211_Event_Transition(fsmLocal, isMsgAIFS(msg) && !transmissionQueue()->empty() && !isMulticast(getCurrentTransmission())
            && isMpduA(getCurrentTransmission())  && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Transmit - RTS \n";
        sendRTSFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        if (useRtsMpduA)
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Transmit - RTS \n";
            FSMIEEE80211_Transition(fsmLocal,WAITCTS);
        }
        else
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Transmit - MPDU \n";
            FSMIEEE80211_Transition(fsmLocal,SENDMPDUA);
        }
    }

    FSMIEEE80211_Event_Transition(fsmLocal, isMsgAIFS(msg) && !transmissionQueue()->empty() && !isMulticast(getCurrentTransmission())
            && getCurrentTransmission()->getByteLength() >= rtsThreshold && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Transmit - RTS \n";
        sendRTSFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITCTS);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isMsgAIFS(msg) && isMulticast(getCurrentTransmission()) && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Transmit - Multicast \n";
        sendMulticastFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITMULTICAST);
    }


    FSMIEEE80211_Event_Transition(fsmLocal,isMsgAIFS(msg) && !isMulticast(getCurrentTransmission()) && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate - Transmit - Data \n";
        sendDataFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITACK);
    }


    FSMIEEE80211_Event_Transition(fsmLocal, isMsgAIFS(msg) || msg == endDIFS)
    {
        if (isMsgAIFS(msg))
        {
            if (fsmLocal->debug()) EV_DEBUG  << "AIFS - Over \n";
        }
        else // msg == endDIFS && !transmissionQueueEmpty()
        {
            if (fsmLocal->debug()) EV_DEBUG  << "DIFS - Over \n";

            if (transmissionQueueEmpty())
                currentAC = numCategories() - 1;
            else
            {
                for (int i = numCategories() - 1; i >= 0; i--)
                {
                    if (!transmissionQueue(i)->empty())
                    {
                        currentAC = i;
                    }
                }
            }
        }
        if (isInvalidBackoffPeriod())
            generateBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,BACKOFF);
    }

    FSMIEEE80211_Event_Transition(fsmLocal, isMediumStateChange(msg) && !isMediumFree())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Busy \n";
        for (int i = 0; i < numCategories(); i++)
        {
            if (endAIFS(i)->isScheduled())
                backoff(i) = true;
        }
        if (endDIFS->isScheduled())
            backoff(numCategories() - 1) = true;
        cancelAIFSPeriod();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal, !isMediumFree())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Immediate Busy \n";
        for (int i = 0; i < numCategories(); i++)
        {
            if (endAIFS(i)->isScheduled())
                backoff(i) = true;
        }
        if (endDIFS->isScheduled())
            backoff(numCategories() - 1) = true;
        cancelAIFSPeriod();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isLowerMessage(msg))
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive \n";
        FSMIEEE80211_Transition(fsmLocal,RECEIVE);
    }
}

void Ieee80211Mac::stateBackoff(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{

    FSMIEEE80211_Enter(fsmLocal)
    {
        scheduleBackoffPeriod();
    }

    if (getCurrentTransmission())
    {
        FSMIEEE80211_Event_Transition(fsmLocal,msg == endBackoff() && !isMulticast(getCurrentTransmission())
                && getCurrentTransmission()->getByteLength() >= rtsThreshold)
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Transmit-RTS \n";
            sendRTSFrame(getCurrentTransmission());
            oldcurrentAC = currentAC;
            cancelAIFSPeriod();
            decreaseBackoffPeriod();
            cancelBackoffPeriod();
            FSMIEEE80211_Transition(fsmLocal,WAITCTS);
        }
        FSMIEEE80211_Event_Transition(fsmLocal,msg == endBackoff() && isMulticast(getCurrentTransmission()))
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Transmit-Multicast \n";
            sendMulticastFrame(getCurrentTransmission());
            oldcurrentAC = currentAC;
            cancelAIFSPeriod();
            decreaseBackoffPeriod();
            cancelBackoffPeriod();
            FSMIEEE80211_Transition(fsmLocal,WAITMULTICAST);
        }
        FSMIEEE80211_Event_Transition(fsmLocal,msg == endBackoff() && !isMulticast(getCurrentTransmission()))
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Transmit-Data \n";
            sendDataFrame(getCurrentTransmission());
            oldcurrentAC = currentAC;
            cancelAIFSPeriod();
            decreaseBackoffPeriod();
            cancelBackoffPeriod();
            FSMIEEE80211_Transition(fsmLocal,WAITACK);
        }
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isMsgAIFS(msg) && backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "AIFS-Over-backoff \n";
        if (isInvalidBackoffPeriod())
            generateBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,BACKOFF);
    }

    FSMIEEE80211_Event_Transition(fsmLocal, isMsgAIFS(msg) && !transmissionQueue()->empty() && !isMulticast(getCurrentTransmission())
            && getCurrentTransmission()->getByteLength() >= rtsThreshold && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "AIFS- Immediate - Transmit-RTS \n";
        sendRTSFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        decreaseBackoffPeriod();
        cancelBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITCTS);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isMsgAIFS(msg) && isMulticast(getCurrentTransmission()) && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "AIFS- Immediate - Transmit-Multicast \n";
        sendMulticastFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        decreaseBackoffPeriod();
        cancelBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITMULTICAST);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isMsgAIFS(msg) && !isMulticast(getCurrentTransmission()) && !backoff())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "AIFS- Immediate - Transmit-Data \n";
        sendDataFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        cancelAIFSPeriod();
        decreaseBackoffPeriod();
        cancelBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITACK);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isBackoffMsg(msg) && transmissionQueueEmpty())
    {
         if (fsmLocal->debug()) EV_DEBUG  << "Backoff-Idle \n";
         resetStateVariables();
         FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_Event_Transition(fsmLocal, isMediumStateChange(msg) && !isMediumFree())
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Backoff-Busy \n";
        cancelAIFSPeriod();
        decreaseBackoffPeriod();
        cancelBackoffPeriod();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
}


void Ieee80211Mac::stateWaitAck(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        scheduleDataTimeoutPeriod(getCurrentTransmission());
    }
    Ieee80211Frame *frame = dynamic_cast<Ieee80211Frame*>(msg);
    bool receptionError = false;
    if (frame && isLowerMessage(frame))
        receptionError = frame ? frame->hasBitError() : false;
    int frameType = frame ? frame->getType() : -1;

#ifndef DISABLEERRORACK
    FSMIEEE80211_Event_Transition(fsmLocal,
            isLowerMessage(msg) && receptionError && retryCounter(oldcurrentAC) == transmissionLimit - 1)
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Reception-ACK-failed \n";
        currentAC = oldcurrentAC;
        cancelTimeoutPeriod();
        giveUpCurrentTransmission();
        txop = false;
        if (endTXOP->isScheduled()) cancelEvent(endTXOP);
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }
    FSMIEEE80211_Event_Transition(fsmLocal, isLowerMessage(msg) && receptionError)
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Reception-ACK-error \n";
        currentAC=oldcurrentAC;
        cancelTimeoutPeriod();
        retryCurrentTransmission();
        txop = false;
        if (endTXOP->isScheduled()) cancelEvent(endTXOP);
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
#endif
    FSMIEEE80211_Event_Transition(fsmLocal,
                                  isLowerMessage(msg) && isForUs(frame) && frameType == ST_ACK && txop && transmissionQueue(oldcurrentAC)->size() == 1)
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive-ACK-TXOP-Empty \n";
        sendNotification(NF_TX_ACKED);// added by aaq
        currentAC = oldcurrentAC;
        if (retryCounter() == 0) numSentWithoutRetry()++;
        numSent()++;
        fr = getCurrentTransmission();
        numBits += fr->getBitLength();
        bits() += fr->getBitLength();
        macDelay()->record(simTime() - fr->getMACArrive());
        if (maxJitter() == SIMTIME_ZERO || maxJitter() < (simTime() - fr->getMACArrive()))
            maxJitter() = simTime() - fr->getMACArrive();
        if (minJitter() == SIMTIME_ZERO || minJitter() > (simTime() - fr->getMACArrive()))
            minJitter() = simTime() - fr->getMACArrive();
        if (fsmLocal->debug()) EV_DEBUG << "record macDelay AC" << currentAC << " value " << simTime() - fr->getMACArrive() <<endl;
        numSentTXOP++;
        cancelTimeoutPeriod();
        finishCurrentTransmission();
        resetCurrentBackOff();
        txop = false;
        if (endTXOP->isScheduled()) cancelEvent(endTXOP);
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                                  isLowerMessage(msg) && isForUs(frame) && frameType == ST_ACK && txop)
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive-ACK-TXOP \n";
        sendNotification(NF_TX_ACKED);// added by aaq
        currentAC = oldcurrentAC;
        if (retryCounter() == 0) numSentWithoutRetry()++;
        numSent()++;
        fr = getCurrentTransmission();
        numBits += fr->getBitLength();
        bits() += fr->getBitLength();


        macDelay()->record(simTime() - fr->getMACArrive());
        if (maxJitter() == SIMTIME_ZERO || maxJitter() < (simTime() - fr->getMACArrive()))
            maxJitter() = simTime() - fr->getMACArrive();
        if (minJitter() == SIMTIME_ZERO || minJitter() > (simTime() - fr->getMACArrive()))
            minJitter() = simTime() - fr->getMACArrive();
        if (fsmLocal->debug()) EV_DEBUG << "record macDelay AC" << currentAC << " value " << simTime() - fr->getMACArrive() <<endl;
        numSentTXOP++;
        cancelTimeoutPeriod();
        finishCurrentTransmission();

        FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
    }
             /*Ieee 802.11 2007 9.9.1.2 EDCA TXOPs*/
    FSMIEEE80211_Event_Transition(fsmLocal,
                                  isLowerMessage(msg) && isForUs(frame) && frameType == ST_ACK)
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive-ACK \n";
        sendNotification(NF_TX_ACKED);// added by mhn **************************************
        currentAC = oldcurrentAC;
        if (retryCounter() == 0)
            numSentWithoutRetry()++;
        numSent()++;
        fr = getCurrentTransmission();
        numBits += fr->getBitLength();
        bits() += fr->getBitLength();

        macDelay()->record(simTime() - fr->getMACArrive());
        if (maxJitter() == SIMTIME_ZERO || maxJitter() < (simTime() - fr->getMACArrive()))
            maxJitter() = simTime() - fr->getMACArrive();
        if (minJitter() == SIMTIME_ZERO || minJitter() > (simTime() - fr->getMACArrive()))
            minJitter() = simTime() - fr->getMACArrive();
        if (fsmLocal->debug()) EV_DEBUG << "record macDelay AC" << currentAC << " value " << simTime() - fr->getMACArrive() <<endl;
        cancelTimeoutPeriod();
        finishCurrentTransmission();
        resetCurrentBackOff();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                                  msg == endTimeout && retryCounter(oldcurrentAC) == transmissionLimit - 1)
    {
        currentAC = oldcurrentAC;
        giveUpCurrentTransmission();
        txop = false;
        if (endTXOP->isScheduled()) cancelEvent(endTXOP);
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                                  msg == endTimeout)
    {
        if (fsmLocal->debug()) EV_DEBUG  << "Receive-ACK-Timeout \n";
        currentAC = oldcurrentAC;
        retryCurrentTransmission();
        txop = false;
        if (endTXOP->isScheduled()) cancelEvent(endTXOP);
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,isLowerMessage(msg))
    {
        currentAC=oldcurrentAC;
        cancelTimeoutPeriod();
        if (retryCounter(oldcurrentAC) == transmissionLimit - 1)
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Interrupted-ACK-Failure \n";
            giveUpCurrentTransmission();
        }
        else
        {
            if (fsmLocal->debug()) EV_DEBUG  << "Retry-Interrupted-ACK \n";
            retryCurrentTransmission();
        }
        txop = false;
        if (endTXOP->isScheduled()) cancelEvent(endTXOP);
        FSMIEEE80211_Transition(fsmLocal,RECEIVE);
    }
}

void Ieee80211Mac::stateWaitBlockAck(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{

    Ieee80211Frame *frame = dynamic_cast<Ieee80211Frame*>(msg);
    bool receptionError = false;
    if (frame && isLowerMessage(frame))
        receptionError = frame ? frame->hasBitError() : false;
    int frameType = frame ? frame->getType() : -1;

    FSMIEEE80211_Enter(fsmLocal)
    {
        if (mpduInTransmission == nullptr)
            FSMIEEE80211_Transition(fsmLocal,DEFER);
        Ieee80211BlockAckFrameReq rreqAck;
        setBlockAckTimeOut();
    }

    FSMIEEE80211_Event_Transition(fsmLocal, isLowerMessage(msg) && receptionError)
    {
          if (fsmLocal->debug()) EV_DEBUG  << "Reception-BLOCK ACK-failed \n";
          currentAC = oldcurrentAC;
          cancelTimeoutPeriod();

          txop = false;
          if (endTXOP->isScheduled()) cancelEvent(endTXOP);
          if ( retryMpduA == transmissionLimit - 1)
          {
             mpduInTransmission = nullptr;
             giveUpCurrentTransmission();
             FSMIEEE80211_Transition(fsmLocal,IDLE);
          }
          else
          {
              retryCurrentMpduA();
              FSMIEEE80211_Transition(fsmLocal,DEFER);
          }
      }

      FSMIEEE80211_Event_Transition(fsmLocal,
                                    isLowerMessage(msg) && isForUs(frame) && frameType == ST_BLOCKACK)
      {

          if (fsmLocal->debug()) EV_DEBUG  << "Receive-ACK-TXOP-Empty \n";
          sendNotification(NF_TX_ACKED);// added by aaq
          processBlockAckFrame(frame); //
          currentAC = oldcurrentAC;
          if (endTXOP->isScheduled()) cancelEvent(endTXOP);

          if (mpduInTransmission)
          {
              FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
          }
          else
          {
              // no more frames
              FSMIEEE80211_Transition(fsmLocal,DEFER);
          }
      }

      FSMIEEE80211_Event_Transition(fsmLocal, msg == endTimeout)
      {
          if (fsmLocal->debug()) EV_DEBUG  << "Receive-BLOCK_ACK-Timeout \n";
          currentAC = oldcurrentAC;

          if (endTXOP->isScheduled()) cancelEvent(endTXOP);

          // check rertries
          giveUpCurrentTransmission();
          txop = false;
          if (retryMpduA == transmissionLimit - 1)
          {
              giveUpCurrentTransmission();
              FSMIEEE80211_Transition(fsmLocal,IDLE);
          }
          else
          {
              retryCurrentTransmission();
              FSMIEEE80211_Transition(fsmLocal,DEFER);
          }
      }


      FSMIEEE80211_Event_Transition(fsmLocal,isLowerMessage(msg))
      {
          currentAC=oldcurrentAC;
          cancelTimeoutPeriod();
          if (retryCounter(oldcurrentAC) == transmissionLimit - 1)
          {
              if (fsmLocal->debug()) EV_DEBUG  << "Interrupted-ACK-Failure \n";
              giveUpCurrentTransmission();
          }
          else
          {
              if (fsmLocal->debug()) EV_DEBUG  << "Retry-Interrupted-ACK \n";
              retryCurrentTransmission();
          }
          txop = false;
          if (endTXOP->isScheduled()) cancelEvent(endTXOP);
          FSMIEEE80211_Transition(fsmLocal,RECEIVE);
      }
}

void Ieee80211Mac::stateSendMpuA(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    Ieee80211MpduA *frame;
    FSMIEEE80211_Enter(fsmLocal)
    {

        if (endSIFS == msg)
        {
            Ieee80211Frame *frameAux = (Ieee80211Frame *)endSIFS->getContextPointer();
            ASSERT(frameAux != nullptr);
            frame = check_and_cast<Ieee80211MpduA *>(frameAux);
        }
        else
        {
            frame = check_and_cast<Ieee80211MpduA *>(msg);
        }

        scheduleDataTimeoutPeriod(frame); // TODO: compute Schedule MPUA with immediate block ACK


        MpduModeTranssmision = true;
        indexMpduTransmission = 0;
        configureRadioMode(IRadio::RADIO_MODE_TRANSMITTER);
        if (mpduInTransmission == nullptr)
            mpduInTransmission = frame;

        for (unsigned int i = 0 ; i < mpduInTransmission->getNumEncap(); i++)
            mpduInTransmission->getPacket(i)->setIsMpduA(true);

        for (unsigned int i = 0 ; i < mpduInTransmission->getNumEncap()-1; i++)
        {
            mpduInTransmission->getPacket(i)->setLastMpdu(false);
        }

        mpduInTransmission->getPacket(mpduInTransmission->getNumEncap()-1)->setLastMpdu(true);
        int retry = mpduInTransmission->getNumRetries(indexMpduTransmission);
        sendDown(buildMpduDataFrame(check_and_cast<Ieee80211DataOrMgmtFrame *>(setBitrateFrame(mpduInTransmission->getPacket(indexMpduTransmission))),retry));
    }

    if (msg == mediumStateChange && radio->getTransmissionState() == IRadio::TRANSMISSION_STATE_IDLE)
    {
        // send next
        indexMpduTransmission++;
        currentAC = oldcurrentAC;
        int retry = mpduInTransmission->getNumRetries(indexMpduTransmission);
        if ((int)mpduInTransmission->getNumEncap() < indexMpduTransmission)
            sendDown(buildMpduDataFrame(check_and_cast<Ieee80211DataOrMgmtFrame *>(setBitrateFrame(mpduInTransmission->getPacket(indexMpduTransmission))),retry));
        else
        {
            // transmit BlockAck request.
            Ieee80211BlockAckFrameReq reqFrame;
            reqFrame.setReceiverAddress(mpduInTransmission->getReceiverAddress());
            reqFrame.setTransmitterAddress(address);
            setBitrateFrame(&reqFrame);
            sendDown(buildMpduDataFrame(&reqFrame,0));
            FSMIEEE80211_Transition(fsmLocal,WAITBLOCKACK);
        }
    }
 }



void Ieee80211Mac::stateWaitMulticast(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        scheduleMulticastTimeoutPeriod(getCurrentTransmission());
    }
    FSMIEEE80211_Event_Transition(fsmLocal, msg == endTimeout)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-Multicast \n";
        currentAC = oldcurrentAC;
        fr = getCurrentTransmission();
        numBits += fr->getBitLength();
        bits() += fr->getBitLength();
        finishCurrentTransmission();
        numSentMulticast++;
        resetCurrentBackOff();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
}

void Ieee80211Mac::stateWaitCts(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        scheduleCTSTimeoutPeriod();
    }
    Ieee80211Frame *frame = dynamic_cast<Ieee80211Frame*>(msg);
    bool receptionError = false;
    if (frame && isLowerMessage(frame))
        receptionError = frame ? frame->hasBitError() : false;
    int frameType = frame ? frame->getType() : -1;

#ifndef DISABLEERRORACK
    FSMIEEE80211_Event_Transition(fsmLocal,
                                   isLowerMessage(msg) && receptionError && retryCounter(oldcurrentAC) == transmissionLimit - 1)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Reception-CTS-Failed \n";
        cancelTimeoutPeriod();
        currentAC = oldcurrentAC;
        giveUpCurrentTransmission();
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }
    FSMIEEE80211_Event_Transition(fsmLocal,
                                   isLowerMessage(msg) && receptionError)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Reception-CTS-error \n";
        cancelTimeoutPeriod();
        currentAC = oldcurrentAC;
        retryCurrentTransmission();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
#endif
    FSMIEEE80211_Event_Transition(fsmLocal,
                                  isLowerMessage(msg) && isForUs(frame) && frameType == ST_CTS)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Receive-CTS \n";
        cancelTimeoutPeriod();
        FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                                  msg == endTimeout && retryCounter(oldcurrentAC) == transmissionLimit - 1)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-RTS-Failed \n";
        currentAC = oldcurrentAC;
        giveUpCurrentTransmission();
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                                  msg == endTimeout)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Receive-CTS-Timeout \n";
        currentAC = oldcurrentAC;
        retryCurrentTransmission();
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }
}


void Ieee80211Mac::stateWaitSift(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{
    FSMIEEE80211_Enter(fsmLocal)
    {
        Ieee80211Frame *frame = dynamic_cast<Ieee80211Frame*>(msg);
        scheduleSIFSPeriod(frame);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                       msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_ACK)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-Data-TXOP \n";
        sendDataFrame(getCurrentTransmission());
        oldcurrentAC = currentAC;
        FSMIEEE80211_Transition(fsmLocal,WAITACK);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                       msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_BLOCKACK_REQ)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-BLOCKACK \n";
        sendBLOCKACKFrameOnEndSIFS();
        oldcurrentAC = currentAC;
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                       msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_BLOCKACK && mpduInTransmission == nullptr)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-Mpdu-A-END \n";
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                       msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_BLOCKACK && mpduInTransmission != nullptr)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-MPDU-A \n";
        FSMIEEE80211_Transition(fsmLocal,SENDMPDUA);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
            msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_RTS)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-CTS \n";
        sendCTSFrameOnEndSIFS();
        finishReception();
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
                       msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_CTS && isMpduA(getCurrentTransmission()))
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-DATA MPDUA\n";
        oldcurrentAC = currentAC;
        FSMIEEE80211_Transition(fsmLocal,SENDMPDUA);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
            msg == endSIFS && getFrameReceivedBeforeSIFS()->getType() == ST_CTS)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-DATA \n";
        sendDataFrameOnEndSIFS(getCurrentTransmission());
        oldcurrentAC = currentAC;
        FSMIEEE80211_Transition(fsmLocal,WAITACK);
    }

    FSMIEEE80211_Event_Transition(fsmLocal,
            msg == endSIFS && isDataOrMgmtFrame(getFrameReceivedBeforeSIFS()))
    {
        if (fsmLocal->debug()) EV_DEBUG << "Transmit-ACK \n";
        sendACKFrameOnEndSIFS();
        finishReception();
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }
}

void Ieee80211Mac::stateReceive(Ieee802MacBaseFsm * fsmLocal,cMessage *msg)
{

    Ieee80211Frame *frame = dynamic_cast<Ieee80211Frame*>(msg);

    bool receptionError = false;
    if (frame && isLowerMessage(frame))
        receptionError = frame ? frame->hasBitError() : false;
    Ieee80211DataOrMgmtFrame *dataFrame = dynamic_cast<Ieee80211DataOrMgmtFrame*>(frame);

    int frameType = frame ? frame->getType() : -1;

    FSMIEEE80211_No_Event_Transition(fsmLocal, dataFrame != nullptr && isLowerMessage(msg)  && isForUs(frame) && dataFrame->getIsMpduA())
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-MPDUA \n";
        processMpduA(dataFrame);
        // finishReception();
        // FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
        FSMIEEE80211_Transition(fsmLocal,IDLE);// IDLE->RECV,
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && receptionError)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-Error \n";
        EV << "received frame contains bit errors or collision, next wait period is EIFS\n";
        numCollision++;
        finishReception();
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && isMulticast(frame) && !isSentByUs(frame) && isDataOrMgmtFrame(frame))
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-Multicast \n";
        sendUp(frame);
        numReceivedMulticast++;
        finishReception();
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && isForUs(frame) && isDataOrMgmtFrame(frame))
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-Data \n";
        sendUp(frame);
        numReceived++;

        FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && isForUs(frame) && frameType == ST_RTS)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-RTS \n";
        FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && isForUs(frame) && frameType == ST_BLOCKACK_REQ)
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-BLOCKACK_REQ \n";
        FSMIEEE80211_Transition(fsmLocal,WAITSIFS);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && isBackoffPending())
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-Other-backtobackoff \n";
        FSMIEEE80211_Transition(fsmLocal,DEFER);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg) && !isForUs(frame) && isDataOrMgmtFrame(frame))
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Promiscuous-Data \n";
        promiscousFrame(frame);
        finishReception();
        numReceivedOther++;

        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }

    FSMIEEE80211_No_Event_Transition(fsmLocal,
                    isLowerMessage(msg))
    {
        if (fsmLocal->debug()) EV_DEBUG << "Immediate-Receive-Other \n";
        finishReception();
        numReceivedOther++;
        FSMIEEE80211_Transition(fsmLocal,IDLE);
    }
}

void Ieee80211Mac::handleWithFSM(cMessage *msg)
{

    bool receptionError;
    Ieee80211Frame *frame;
    if (!initFsm(msg, receptionError, frame))
        return;
    fsm->execute(msg);
    endFsm(msg);
}


}

}