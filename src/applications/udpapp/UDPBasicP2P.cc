//
// Copyright (C) 2012 Alfonso Ariza, Universidad de Malaga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//


#include "UDPBasicP2P.h"
#include "UDPControlInfo_m.h"
#include "IPvXAddressResolver.h"
#include "InterfaceTable.h"
#include "InterfaceTableAccess.h"
#include "GlobalWirelessLinkInspector.h"
#include <algorithm>
#include "MobilityAccess.h"
#include "IPv4InterfaceData.h"
#include "RoutingTableAccess.h"

Define_Module(UDPBasicP2P);

simsignal_t UDPBasicP2P::sentPkSignal = registerSignal("sentPk");
simsignal_t UDPBasicP2P::rcvdPkSignal = registerSignal("rcvdPk");
simsignal_t UDPBasicP2P::outOfOrderPkSignal = registerSignal("outOfOrderPk");
simsignal_t UDPBasicP2P::dropPkSignal = registerSignal("dropPk");
simsignal_t UDPBasicP2P::sentPkSignalP2p = registerSignal("sentPkP2p");
simsignal_t UDPBasicP2P::rcvdPkSignalP2p = registerSignal("rcvdPkP2p");

simsignal_t UDPBasicP2P::queueP2pLengthSignal = registerSignal("queueP2pLength");


UDPBasicP2P::SegmentMap UDPBasicP2P::segmentMap;

UDPBasicP2P::VectorList UDPBasicP2P::vectorList;

UDPBasicP2P::InverseAddres UDPBasicP2P::inverseAddress;
UDPBasicP2P::DirectAddres UDPBasicP2P::directAddress;

std::vector<int> UDPBasicP2P::initNodes;
int UDPBasicP2P::counter;

EXECUTE_ON_STARTUP(
    cEnum *e = cEnum::find("modoP2P");
    if (!e) enums.getInstance()->add(e = new cEnum("modoP2P"));
    e->insert(UDPBasicP2P::CONCAVO, "concavo");
    e->insert(UDPBasicP2P::ADITIVO, "aditivo");
    e->insert(UDPBasicP2P::ADITIVO_PONDERADO, "aditivo_ponderado");
    e->insert(UDPBasicP2P::MIN_HOP, "minhop");
    e->insert(UDPBasicP2P::RANDOM, "random");
);


UDPBasicP2P::UDPBasicP2P()
{
    myTimer = new cMessage("UDPBasicP2P-timer");
    queueTimer = new cMessage("UDPBasicP2P-queueTimer");
    fuzzy = NULL;
    numSent = 0;
    numReceived = 0;
    numDeleted = 0;
    numDuplicated = 0;
    numSegPresent = 0;
    parallelConnection.clear();
    vectorList.clear();
    writeData = false;
    numRegData = 0;
    outfile = NULL;
    inverseAddress.clear();
    directAddress.clear();
    numRequestSent = 0;
    numRequestServed = 0;
    numRequestSegmentServed = 0;
    initNodes.clear();
    routing = NULL;

    numSentB = 0;
    numReceivedB = 0;
    numDeletedB = 0;
    numDuplicatedB = 0;


    messageLengthPar = NULL;
    burstDurationPar = NULL;
    sleepDurationPar = NULL;
    sendIntervalPar = NULL;
    timerNext = NULL;
    outputInterface = -1;
    outputInterfaceMulticastBroadcast.clear();
}

UDPBasicP2P::~UDPBasicP2P()
{
    cancelAndDelete(myTimer);
    cancelAndDelete(queueTimer);
    cancelAndDelete(timerNext);
    cancelEvent(&periodicTimer);
    while(!timeQueue.empty())
    {
        delete timeQueue.begin()->second;
        timeQueue.erase(timeQueue.begin());
    }
    SegmentMap::iterator it = segmentMap.find(myAddress.getMAC().getInt());
    if (it != segmentMap.end())
        segmentMap.erase(it);
    mySegmentList.clear();
    clientList.clear();
    for (ParallelConnection::iterator it = parallelConnection.begin();it != parallelConnection.end();++it)
    {
        cancelEvent(&it->timer);
        it->segmentInTransit.clear();
    }

    parallelConnection.clear();
    if (outfile)
    {
        outfile->close();
        delete outfile;
    }
    if (routing)
        delete routing;
}

void UDPBasicP2P::initialize(int stage)
{

    // because of IPvXAddressResolver, we need to wait until interfaces are registered,
    // address auto-assignment takes place etc.
    if (stage == 6)
    {
       WirelessNumNeig();
       if (par("appRouting").boolValue())
       {
           routing = new WirelessNumHops();
           routing->setRoot(myAddress.getMAC());
       }
    }

    if (stage != 5)
        return;

    const char *addrModeStr = par("modoP2P").stringValue();
    int p2pMode = cEnum::get("modoP2P")->lookup(addrModeStr);
    if (p2pMode == -1)
        throw cRuntimeError("Invalid modoP2P: '%s'", addrModeStr);
    modo = (modoP2P)p2pMode;

    if (!GlobalWirelessLinkInspector::isActive())
        opp_error("GlobalWirelessLinkInspector not found");

    if (par("fuzzy"))
    {
        fuzzy = new FuzzYControl(par("fuzzyFisFile").stringValue());
        fuzzy->setSizeParam(par("numVarFuzzy").longValue());
    }

    totalSegments = par("totalSegments").longValue();
    segmentSize = par("segmentSize").longValue();

    uint64_t initseg = 0;
    IPvXAddress myAddr = IPvXAddressResolver().resolve(this->getParentModule()->getFullPath().c_str());
    IInterfaceTable * ift = IPvXAddressResolver().interfaceTableOf(this->getParentModule());

    myAddress = ManetAddress::ZERO;
    for (int i = 0; i< ift->getNumInterfaces();i++)
    {
        InterfaceEntry * e = ift->getInterface(i);
        if (e->ipv4Data())
        {
            if (e->ipv4Data()->getIPAddress() == myAddr.get4())
            {
                myAddress = ManetAddress(e->getMacAddress());
                break;
            }
        }
    }
    if (myAddr.get4() == IPv4Address::UNSPECIFIED_ADDRESS)
        opp_error("addr invalid");
    if (myAddress == ManetAddress::ZERO)
        opp_error("MACAddress addr invalid");
    inverseAddress[myAddress.getMAC().getInt()] = myAddr.get4();
    directAddress[myAddr.get4()] = myAddress.getMAC().getInt();


    if (!par("initNodesRand").boolValue())
        initseg = par("numInitialSegments").longValue();
    else
    {
        // cModule * myNode = IPvXAddressResolver().findHostWithAddress(myAddr);
        cModule * myNode = this->getParentModule();
        if (!myNode->isVector())
            error("No es un vector");
        if (initNodes.empty())
        {
            int numNodes = myNode->getVectorSize();
            while ((int)initNodes.size() < par("numInitNodesRand").longValue())
            {
                int val = intuniform(0,numNodes);
                bool isInside = false;
                for (int i = 0; i < (int) initNodes.size(); i++)
                {
                    if (val == initNodes[i])
                    {
                        isInside = true;
                        break;
                    }
                }
                if (isInside)
                    continue;
                initNodes.push_back(val);
            }
        }
        int myIndex = myNode->getIndex();
        for (int i = 0; i < (int) initNodes.size(); i++)
        {
            if (myIndex == initNodes[i])
            {
                initseg = totalSegments;
                break;
            }
        }
    }
    maxPacketSize = par("maxPacketSize");
    rateLimit = par("packetRate");
    requestPerPacket = par("requestPerPacket");
    lastPacket = 0;
    serverTimer = par("serverRate");


    if (totalSegments == initseg)
    {
        for (unsigned int i = 0; i < totalSegments; i++)
            mySegmentList.insert(i);
    }
    else if (initseg == 0)
    {
        mySegmentList.clear();
    }
    else
    {
        // random initialization
        while (mySegmentList.size()<initseg)
        {
            mySegmentList.insert(intuniform(0,initseg));
        }
    }

    segmentMap[myAddress.getMAC().getInt()] = &mySegmentList;

    if (mySegmentList.size()<totalSegments)
    {
        startReception = par("startTime");
        endReception = SimTime::getMaxTime();
        scheduleAt(startReception, myTimer);
    }
    else
    {
        startReception = 0;
        endReception = 0;
    }

    if (par("periodicTimer").doubleValue() > 0)
        scheduleAt(simTime()+par("periodicTimer"), &periodicTimer);

    localPort = par("localPort");
    destPort = par("destPort");
    request.resize(totalSegments,0);

    socket.setOutputGate(gate("udpOut"));
    socket.bind(localPort);
    numSegPresent = (int)mySegmentList.size();

    numParallelRequest = par("numParallelRequest");

    clientList.clear();
    timeQueue.clear();
    WATCH(numSegPresent);
   // WATCH((unsigned int)timeQueue.size());



    const char *listNodes = par("listNodes");
    cStringTokenizer tokenizer(listNodes);
    const char *token;

    while ((token = tokenizer.nextToken()) != NULL)
    {
        IPvXAddress addr = IPvXAddressResolver().resolve(token);
        if (addr == myAddr)
        {
            writeData = true;
            std::string name(token);
            outfile = new std::ofstream(name.c_str(), std::ios_base::out | std::ios_base::trunc | std::ios_base::app);
            *outfile << simulation.getActiveSimulation()->getEnvir()->getConfigEx()->getVariable(CFGVAR_SEEDSET) << endl;
        }
    }
}


uint64_t UDPBasicP2P::chooseDestAddr()
{
    std::vector<uint64_t> address;
    getList(address);
    return selectBest(address);
}


uint64_t UDPBasicP2P::chooseDestAddr(uint32_t segmentId)
{
    std::vector<uint64_t> address;
    getList(address,segmentId);
    return selectBest(address);
}


std::vector<UDPBasicP2P::InfoData>  UDPBasicP2P::chooseDestAddrList()
{
    std::vector<uint64_t> address;
    std::vector<SegmentList> Segments;
    getList(address,Segments);
    return selectBestList(address, Segments);
}

bool UDPBasicP2P::areDiff(const SegmentList &List1, const SegmentList &List2)
{
    SegmentList result;
    std::set_difference(List1.begin(),List1.end(),List2.begin(),List2.end(),std::inserter(result, result.end()));
    if (!result.empty())
        return true;
    return
        false;
}

std::vector<UDPBasicP2P::InfoData> UDPBasicP2P::selectBestList(const std::vector<uint64_t> &address, const std::vector<SegmentList> &List)
{
    double costMax = 1e300;
    double maxfuzzyCost = 0;
    uint64_t addr = 0;

    std::vector<UDPBasicP2P::InfoData> value;

    ManetAddress myAdd = myAddress;
    std::vector<uint64_t> winners;
    int cont = numParallelRequest;

    for (unsigned int i =0 ;i<address.size(); i++)
    {
//        bool procNode = true;
//        for (unsigned int l = 0; l < winners.size(); l++)
//        {
//
//            if (!areDiff(List[i],List[winners[l]]))
//            {
//                procNode = false;
//                break;
//            }
//        }
//
//        if (!procNode)
//            continue;

        std::vector<ManetAddress> route;
        ManetAddress aux = ManetAddress(MACAddress(address[i]));
        GlobalWirelessLinkInspector::Link cost;
        bool validRoute = false;

        int neigh = 0;
        if (routing)
        {
            std::deque<MACAddress> pathNode;
            //if (routing->findRouteWithCost(par("coverageArea").doubleValue(), MACAddress(address[i]),pathNode,true,costAdd,costMax))
            if (routing->findRoute(par("coverageArea").doubleValue(), MACAddress(address[i]),pathNode))
            {
                InverseAddres::iterator it = inverseAddress.find(pathNode[0].getInt());
                if (it == inverseAddress.end())
                    throw cRuntimeError(" address not found %s", pathNode[0].str().c_str());
                IPv4Address desAdd = it->second;
                validRoute = true;
                route.push_back(myAdd);
                for (unsigned int i = 0; i < pathNode.size(); i++)
                    route.push_back(ManetAddress(pathNode[i]));
            }
        }
        else if (GlobalWirelessLinkInspector::getRouteWithLocator(myAdd, aux, route))
        {
            validRoute = true;
        }


        if(validRoute)
        {
            if (fuzzy)
            {
                int nodesN = -1;
                //nodesN = getNumNeighNodes(address[i],120.0);
                bool areNei;
                GlobalWirelessLinkInspector::areNeighbour(myAdd,aux,areNei);
                if (areNei)
                {
                    InfoData data;
                    winners.push_back(address[i]);
                    cont--;
                }
                // GlobalWirelessLinkInspector::getNumNodes(aux, neigh);
                if (modo == CONCAVO)
                    GlobalWirelessLinkInspector::getWorst(route, cost);
                else
                    GlobalWirelessLinkInspector::getCostPath(route, cost);
                if (modo == ADITIVO_PONDERADO)
                     cost.costEtx/=(route.size()-1);

                std::vector<double> inputData;
                inputData.push_back(route.size()-1);
                inputData.push_back(cost.costEtx);

                if (nodesN > neigh)
                    inputData.push_back(nodesN);
                else
                    inputData.push_back(neigh);

                double fuzzyCost;
                fuzzy->runFuzzy(inputData,fuzzyCost);
                if (writeData)
                {
                    *outfile << numRegData << " ";
                    for (unsigned int i = 0 ; i < inputData.size(); i++)
                        *outfile << inputData[i] << " ";
                    *outfile << fuzzyCost << endl;
                    numRegData++;
                }

                if (par("forceMinHop").boolValue())
                {
                    if (route.size() < costMax)
                    {
                        costMax = route.size();
                        addr = address[i];
                        maxfuzzyCost = fuzzyCost;
                        winners.clear();
                        winners.push_back(addr);
                    }
                    else if (fuzzyCost > maxfuzzyCost && route.size() == costMax)
                    {
                        costMax = route.size();
                        addr = address[i];
                        maxfuzzyCost = fuzzyCost;
                        winners.clear();
                        winners.push_back(addr);
                    }
                    else if (fuzzyCost == maxfuzzyCost && route.size() == costMax)
                    {
                        addr = address[i];
                        winners.push_back(addr);
                    }
                }
                else
                {
                    if (fuzzyCost > maxfuzzyCost)
                    {
                        costMax = route.size();
                        addr = address[i];
                        maxfuzzyCost = fuzzyCost;
                        winners.clear();
                        winners.push_back(addr);
                    }
                    else if (fuzzyCost == maxfuzzyCost)
                    {
                        addr = address[i];
                        winners.push_back(addr);
                    }
                }
            }
            else
            {

                // GlobalWirelessLinkInspector::getNumNodes(aux, neigh);
                double costPath;

                if (modo == MIN_HOP)
                    costPath = route.size();
                else
                {
                    if (modo == CONCAVO)
                        GlobalWirelessLinkInspector::getWorst(route, cost);
                    else
                        GlobalWirelessLinkInspector::getCostPath(route, cost);
                    if (modo == ADITIVO_PONDERADO)
                        cost.costEtx/=(route.size()-1);
                    costPath = cost.costEtx;
                }
                if (costPath < costMax)
                {
                    costMax = costPath;
                    addr = address[i];
                    winners.clear();
                    winners.push_back(addr);
                }
                else if (costPath == costMax)
                {
                    addr = address[i];
                    winners.push_back(addr);
                }
            }
        }
    }
    if (winners.empty())
        error("winners empty");
    else
    {
        int val = intuniform(0,winners.size()-1);
        addr = winners[val];
        for (unsigned int i = 0; i < winners.size(); i++)
        {
            InfoData data;
            data.nodeId = winners[val];
            std::set_difference(mySegmentList.begin(),mySegmentList.end(),List[winners[i]].begin(),List[winners[i]].end(),std::inserter(data.list,data.list.end()));
            value.push_back(data);
        }
    }
    return value;
}


uint64_t UDPBasicP2P::selectBest(const std::vector<uint64_t> &address)
{
    double costMax = 1e300;
    double maxfuzzyCost = 0;
    uint64_t addr = 0;
    ManetAddress myAdd = myAddress;
    std::vector<uint64_t> winners;
    if (modo == UDPBasicP2P::RANDOM)
    {
        unsigned int pos = intuniform(0,address.size()-1);
        return address[pos];
    }

    for (unsigned int i = 0 ;i < address.size(); i++)
    {
        std::vector<ManetAddress> route;
        ManetAddress aux = ManetAddress(MACAddress(address[i]));
        bool validRoute = false;
        GlobalWirelessLinkInspector::Link cost;
        int neigh = 0;
        if (routing)
        {

            std::deque<MACAddress> pathNode;
            //if (routing->findRouteWithCost(par("coverageArea").doubleValue(), aux.getMAC(), pathNode,true,costAdd,costMax))
            if (routing->findRoute(par("coverageArea").doubleValue(), MACAddress(address[i]),pathNode))
            {
                InverseAddres::iterator it = inverseAddress.find(pathNode[0].getInt());
                if (it == inverseAddress.end())
                    throw cRuntimeError(" address not found %s", pathNode[0].str().c_str());
                IPv4Address desAdd = it->second;
                validRoute = true;
                route.push_back(myAdd);
                for (unsigned int i = 0; i < pathNode.size(); i++)
                    route.push_back(ManetAddress(pathNode[i]));
            }
            else
                opp_error("route not found");
        }
        else if (GlobalWirelessLinkInspector::getRouteWithLocator(myAdd, aux, route))
        {
            validRoute = true;
        }

        if (validRoute)
        {
            if (fuzzy)
            {
                int nodesN = -1;
                //nodesN = getNumNeighNodes(address[i],120.0);

#ifdef NEIGH
                bool areNei;
                GlobalWirelessLinkInspector::areNeighbour(myAdd,aux,areNei);
                if (areNei)
                    return address[i];
#endif
                GlobalWirelessLinkInspector::getNumNodes(aux, neigh);
                if (modo == CONCAVO)
                    GlobalWirelessLinkInspector::getWorst(route, cost);
                else
                    GlobalWirelessLinkInspector::getCostPath(route, cost);
                if (modo == ADITIVO_PONDERADO)
                   cost.costEtx/=(route.size()-1);

                std::vector<double> inputData;
                inputData.push_back(route.size()-1);
                inputData.push_back(cost.costEtx);
                if (nodesN > neigh)
                    inputData.push_back(nodesN);
                else
                    inputData.push_back(neigh);

                double fuzzyCost;
                fuzzy->runFuzzy(inputData,fuzzyCost);
                if (writeData)
                {
                    *outfile << numRegData << " ";
                    for (unsigned int i = 0 ; i < inputData.size(); i++)
                        *outfile << inputData[i] << " ";
                    *outfile << fuzzyCost << endl;
                    numRegData++;
                }

                if (par("forceMinHop").boolValue())
                {
                    if (route.size() < costMax)
                    {
                        costMax = route.size();
                        addr = address[i];
                        maxfuzzyCost = fuzzyCost;
                        winners.clear();
                        winners.push_back(addr);
                    }
                    else if (fuzzyCost > maxfuzzyCost && route.size() == costMax)
                    {
                        costMax = route.size();
                        addr = address[i];
                        maxfuzzyCost = fuzzyCost;
                        winners.clear();
                        winners.push_back(addr);
                    }
                    else if (fuzzyCost == maxfuzzyCost && route.size() == costMax)
                    {
                        addr = address[i];
                        winners.push_back(addr);
                    }
                }
                else
                {
                    if (fuzzyCost > maxfuzzyCost)
                    {
                        costMax = route.size();
                        addr = address[i];
                        maxfuzzyCost = fuzzyCost;
                        winners.clear();
                        winners.push_back(addr);
                    }
                    else if (fuzzyCost == maxfuzzyCost)
                    {
                        addr = address[i];
                        winners.push_back(addr);
                    }
                }
            }
            else
            {
                // GlobalWirelessLinkInspector::getNumNodes(aux, neigh);
                double costPath;
                if (modo == MIN_HOP)
                    costPath = route.size();
                else
                {
                    if (modo == CONCAVO)
                        GlobalWirelessLinkInspector::getWorst(route, cost);
                    else
                        GlobalWirelessLinkInspector::getCostPath(route, cost);
                    if (modo == ADITIVO_PONDERADO)
                        cost.costEtx/=(route.size()-1);
                    costPath = cost.costEtx;
                }
                if (costPath < costMax)
                {
                    costMax = costMax;
                    addr = address[i];
                    winners.clear();
                    winners.push_back(addr);
                }
                else if (costPath == costMax)
                {
                    addr = address[i];
                    winners.push_back(addr);
                }
            }
        }
    }
    if (winners.empty())
        error("winners empty");
    else
    {
        int val = intuniform(0,winners.size()-1);
        addr = winners[val];
    }
    return addr;
}

void UDPBasicP2P::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage())
    {

        if (msg == &periodicTimer)
        {
            //emit(queueP2pLengthSignal, timeQueue.size());
            scheduleAt(simTime()+par("periodicTimer"), &periodicTimer);
            return;
        }

        if (msg == queueTimer)
            sendQueue();
        else if (msg == myTimer || strcmp(msg->getName(),"PARALLELTIMER") == 0)
            processMyTimer(msg);
        else
            handleMessageWhenUp(msg);
    }
    else if (msg->getKind() == UDP_I_DATA)
    {

        UDPBasicPacketP2P *pkt = check_and_cast<UDPBasicPacketP2P*>(msg);
        if (pkt->getType() == GENERAL)
        {
            if (routing)
            {
                if (pkt->getDestination() != myAddress)
                {
                    std::deque<MACAddress> pathNode;

                    routing->findRoute(par("coverageArea").doubleValue(), pkt->getDestination().getMAC(),pathNode);
                    InverseAddres::iterator it = inverseAddress.find(pathNode[0].getInt());
                    if (it == inverseAddress.end())
                        throw cRuntimeError(" address not found %s", pathNode[0].str().c_str());
                    IPv4Address desAdd = it->second;
                    if (pkt->getControlInfo())
                        delete pkt->removeControlInfo();
                    socket.sendTo(pkt, desAdd, destPort);
                    return;
                }
            }
            cPacket *pktAux = pkt->decapsulate();
            pktAux->setKind(UDP_I_DATA);
            handleMessageWhenUp(pktAux);
            delete pkt;
            return;
        }

        if (processPacket(pkt))
        {
            if (mySegmentList.size() == totalSegments && endReception != 0)
            {
                endReception = simTime();
            }
            bool endSim = true;
            for (SegmentMap::iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
            {
                if (it->second->size() != totalSegments)
                {
                    endSim = false;
                    break;
                }
            }
            if (endSim)
                endSimulation();
        }
    }
    else if (msg->getKind() == UDP_I_ERROR)
    {
        EV << "Ignoring UDP error report\n";
        delete msg;
    }
    else
    {
        error("Unrecognized message (%s)%s", msg->getClassName(), msg->getName());
    }

    if (!timeQueue.empty() && !queueTimer->isScheduled())
    {
        opp_error("Queue not empty and timer not scheduled");
    }

    if (mySegmentList.size()<totalSegments && !myTimer->isScheduled())
        scheduleAt(simTime()+par("requestTimer"), myTimer);
    numSegPresent = (int)mySegmentList.size();
}

void UDPBasicP2P::sendQueue()
{

    if (timeQueue.empty())
        return;

    if (timeQueue.begin()->first <= simTime())
    {
        DelayMessage *delayM = timeQueue.begin()->second;
        timeQueue.erase(timeQueue.begin());
        InverseAddres::iterator it = inverseAddress.find(delayM->getDestination().getMAC().getInt());
        UDPBasicPacketP2P *pkt = delayM->getPkt();
        pkt->setDestination(delayM->getDestination());
        if (it == inverseAddress.end())
            throw cRuntimeError(" address not found %s", delayM->getDestination().getMAC().str().c_str());
        IPv4Address desAdd = it->second;
        numSent++;
        if (routing)
        {

            std::deque<IPv4Address> pathNode;
            if (routing->findRoute(par("coverageArea").doubleValue(), desAdd,pathNode))
                desAdd = pathNode[0];
            else
                throw cRuntimeError(" route not found not found %s", desAdd.str().c_str());
        }
        emit(sentPkSignalP2p, pkt);
        socket.sendTo(pkt, desAdd, destPort);
        lastPacket = simTime();
        lastServer = simTime();
        delete delayM;
    }
    if (!timeQueue.empty())
        scheduleAt(simTime()+serverTimer,queueTimer);
}

bool UDPBasicP2P::processMyTimer(cMessage *msg)
{

    if (msg == myTimer)
    {
        // lost request;
        generateRequestNew();
        return false;
    }

    ParallelConnection::iterator itPar;
    for (itPar = parallelConnection.begin(); itPar != parallelConnection.end();++itPar)
    {
        if (&(itPar->timer) == msg)
            break;
    }

    if (itPar == parallelConnection.end())
        return true;

    if (itPar->segmentInTransit.empty())
        parallelConnection.erase(itPar);

    numRequestSent++;
    if (parallelConnection.size() < numParallelRequest)
        generateRequestNew();
    else
        generateRequestSub();

    return false;
}

bool UDPBasicP2P::processPacket(cPacket *pk)
{
    if (pk->getKind() == UDP_I_ERROR)
    {
        EV << "UDP error received\n";
        delete pk;
        return false;
    }

    UDPBasicPacketP2P *pkt = check_and_cast<UDPBasicPacketP2P*>(pk);

    if (routing)
    {
        if (pkt->getDestination() != myAddress)
        {
            std::deque<MACAddress> pathNode;

            routing->findRoute(par("coverageArea").doubleValue(), pkt->getDestination().getMAC(),pathNode);
            InverseAddres::iterator it = inverseAddress.find(pathNode[0].getInt());
            if (it == inverseAddress.end())
                throw cRuntimeError(" address not found %s", pathNode[0].str().c_str());
            IPv4Address desAdd = it->second;
            if (pkt->getControlInfo())
                delete pkt->removeControlInfo();
            socket.sendTo(pkt, desAdd, destPort);
            return false;
        }
    }

    bool change = false;
    if (pkt->getType() == REQUEST)
    {
        numRequestServed++;
        processRequest(pk);
    }
    else if (pkt->getType() == SEGMEN)
    {
        if (myTimer->isScheduled())
            cancelEvent(myTimer);
        numRequestSegmentServed++;
        SegmentList::iterator it = mySegmentList.find(pkt->getSegmentId());
        if (it == mySegmentList.end())
        {
            // check sub segments
            if (pkt->getSubSegmentId() == 0)
                mySegmentList.insert(pkt->getSegmentId());
            else
            {
                ParallelConnection::iterator itPar;
                for (itPar = parallelConnection.begin(); itPar != parallelConnection.end();++itPar)
                {
                    if (itPar->segmentId == pkt->getSegmentId())
                        break;
                }

                if (itPar == parallelConnection.end())
                {
                    uint64_t numSubSegments = ceil((double)pkt->getTotalSize()/(double)maxPacketSize);
                    if (pkt->getNodeId() == ManetAddress::ZERO)
                        opp_error("id invalid");
                    ConnectInTransit info(pkt->getNodeId(),pkt->getSegmentId(),numSubSegments,this);

                    parallelConnection.push_back(info);
                    itPar = parallelConnection.end()-1;
                }
                        // erase the subsegment from transit list
                std::set<uint16_t>::iterator itaux = itPar->segmentInTransit.find(pkt->getSubSegmentId());
                if  (itaux != itPar->segmentInTransit.end())
                {
                    itPar->segmentInTransit.erase(itaux);
                }
                if (itPar->segmentInTransit.empty())
                {

                    mySegmentList.insert(pkt->getSegmentId());
                    if (itPar->timer.isScheduled())
                        cancelEvent(&(itPar->timer));
                    parallelConnection.erase(itPar);
                }
                else
                {
                    if (itPar->timer.isScheduled())
                        cancelEvent(&(itPar->timer));
                    scheduleAt(simTime()+par("requestTimer"),&(itPar->timer));
                }

            }
        }
        else
        {
            ParallelConnection::iterator itPar;
            for (itPar = parallelConnection.begin(); itPar != parallelConnection.end();++itPar)
            {
                if (itPar->segmentId == pkt->getSegmentId())
                {
                    if (itPar->timer.isScheduled())
                        cancelEvent(&(itPar->timer));
                    parallelConnection.erase(itPar);
                    break;
                }
            }
            // check that the segment is not in the transit list
        }
        change = true;
        if (numParallelRequest > parallelConnection.size() && totalSegments != mySegmentList.size())
            generateRequestNew();
    }

    EV << "Received packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
    emit(rcvdPkSignalP2p, pk);
    numReceived++;
    delete pk;
    return change;
}

void UDPBasicP2P::generateRequestSub()
{
    if (totalSegments > mySegmentList.size() + parallelConnection.size())
    {
        ParallelConnection::iterator itPar;
        for (itPar = parallelConnection.begin(); itPar != parallelConnection.end();++itPar)
        {
            if (itPar->timer.isScheduled())
                continue;

            uint32_t segmentId = itPar->segmentId;
            if (itPar->segmentInTransit.empty())
                opp_error("");

            uint64_t node = itPar->nodeId.getMAC().getInt();

            if (par("changeSugSegment").boolValue())
            {
                 node = chooseDestAddr(segmentId);
            }
            if (node)
            {
                if (node != itPar->nodeId.getMAC().getInt())
                    itPar->nodeId = ManetAddress(MACAddress(node));
            }
            else
            {
                node = chooseDestAddr(segmentId);
            }

            if (node != itPar->nodeId.getMAC().getInt())
                itPar->nodeId = ManetAddress(MACAddress(node));

            ClientList::iterator it = clientList.find(itPar->nodeId.getMAC().getInt());

            if (it == clientList.end())
            {
                InfoClient infoC;
                infoC.numRequest = 1;
                infoC.numRequestRec = 0;
                clientList.insert(std::pair<uint64_t,InfoClient>(itPar->nodeId.getMAC().getInt(),infoC));
                it = clientList.find(itPar->nodeId.getMAC().getInt());
            }
            std::vector<uint16_t> vectorAux;
            vectorAux.clear();

            for (std::set<uint16_t>::iterator itset = itPar->segmentInTransit.begin(); itset != itPar->segmentInTransit.end(); ++itset)
            {
                vectorAux.push_back(*itset);
                if (vectorAux.size() == requestPerPacket)
                {
                    UDPBasicPacketP2P *pkt = new UDPBasicPacketP2P;
                    pkt->setType(REQUEST);
                    pkt->setSegmentId(segmentId);
                    pkt->setNodeId(myAddress);
                    pkt->setSubSegmentRequestArraySize(vectorAux.size());
                    for (unsigned int j = 0; j<vectorAux.size(); j++)
                        pkt->setSubSegmentRequest(j,vectorAux[j]);
                    pkt->setByteLength(10+(vectorAux.size()*2));
                    pkt->setDestination(itPar->nodeId);
                    sendRateLimit(pkt, itPar->nodeId.getMAC().getInt(), rateLimit);
                    it->second.numRequest++;
                    vectorAux.clear();
                }
            }
            if (!vectorAux.empty())
            {
                UDPBasicPacketP2P *pkt = new UDPBasicPacketP2P;
                pkt->setType(REQUEST);
                pkt->setSegmentId(segmentId);
                pkt->setNodeId(myAddress);
                pkt->setSubSegmentRequestArraySize(vectorAux.size());
                for (unsigned int j = 0; j<vectorAux.size(); j++)
                    pkt->setSubSegmentRequest(j,vectorAux[j]);
                pkt->setByteLength(10+(vectorAux.size()*2));
                pkt->setDestination(itPar->nodeId);
                sendRateLimit(pkt, itPar->nodeId.getMAC().getInt(), rateLimit);
                it->second.numRequest++;
                vectorAux.clear();
            }
            scheduleAt(simTime()+par("requestTimer"),&(itPar->timer));
        }
    }
}


void UDPBasicP2P::generateRequestNew()
{
    if (totalSegments > mySegmentList.size() + parallelConnection.size())
    {
        uint64_t best = chooseDestAddr();
        UDPBasicPacketP2P *pkt = new UDPBasicPacketP2P;
        pkt->setType(REQUEST);
        pkt->setNodeId(myAddress);
        pkt->setByteLength(10);

        ClientList::iterator it = clientList.find(best);
        if (it == clientList.end())
        {

            InfoClient infoC;
            infoC.numRequest = 1;
            infoC.numRequestRec = 0;
            clientList.insert(std::pair<uint64_t,InfoClient>(best,infoC));
            it = clientList.find(best);
        }
        else
            it->second.numRequest++;

        InverseAddres::iterator it2 = inverseAddress.find(best);
        if (it2 == inverseAddress.end())
            throw cRuntimeError("arp->getInverseAddressResolution address not found %s", MACAddress(best).str().c_str());
        IPv4Address desAdd = it2->second;

        numSent++;
        pkt->setDestination(ManetAddress(MACAddress(best)));
        if (routing)
        {
            std::deque<MACAddress> pathNode;
            routing->findRoute(par("coverageArea").doubleValue(), MACAddress(best),pathNode);
            InverseAddres::iterator it = inverseAddress.find(pathNode[0].getInt());
            if (it == inverseAddress.end())
                throw cRuntimeError(" address not found %s", pathNode[0].str().c_str());
            desAdd = it->second;
        }
        if (pkt->getControlInfo())
            delete pkt->removeControlInfo();
        emit(sentPkSignalP2p, pkt);
        socket.sendTo(pkt, desAdd, destPort);
        scheduleAt(simTime()+par("requestTimer"),myTimer);
    }
}


void UDPBasicP2P::receiveChangeNotification(int category, const cObject *details)
{
    Enter_Method_Silent();
}

void UDPBasicP2P::getList(std::vector<uint64_t> &address)
{
    address.clear();
    SegmentList tempMySegmentList(mySegmentList);
    // include segment in transit
    for (ParallelConnection::iterator itPar =  parallelConnection.begin(); itPar != parallelConnection.end();)
    {
        SegmentList::iterator it = mySegmentList.find(itPar->segmentId);

        if (it != mySegmentList.end())
        {
            if (itPar->timer.isScheduled())
                cancelEvent(&(itPar->timer));
            itPar = parallelConnection.erase(itPar);
        }
        else
        {
            tempMySegmentList.insert(itPar->segmentId);
            ++itPar;
        }
    }

    for (SegmentMap::iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
    {
        if (it->first == myAddress.getMAC().getInt())
            continue;
        SegmentList * nodeSegmentList = it->second;
        SegmentList result;
        std::set_difference(nodeSegmentList->begin(),nodeSegmentList->end(),tempMySegmentList.begin(),tempMySegmentList.end(),std::inserter(result, result.end()));
        if (!result.empty())
            address.push_back(it->first);
    }
}


void UDPBasicP2P::getList(std::vector<uint64_t> &address,uint32_t segmentId)
{
    address.clear();
    for (SegmentMap::iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
    {
        if (it->first == myAddress.getMAC().getInt())
            continue;
        SegmentList * nodeSegmentList = it->second;
        SegmentList result;
        SegmentList::iterator it2 = nodeSegmentList->find(segmentId);
        if (it2 != nodeSegmentList->end())
            address.push_back(it->first);
    }
}


void UDPBasicP2P::getList(std::vector<uint64_t> &address, std::vector<SegmentList> &segmentData)
{
    address.clear();
    segmentData.clear();
    for (SegmentMap::iterator it = segmentMap.begin(); it != segmentMap.end(); ++it)
    {
        if (it->first == myAddress.getMAC().getInt())
            continue;
        SegmentList * nodeSegmentList = it->second;
        SegmentList result;
        std::set_difference(nodeSegmentList->begin(),nodeSegmentList->end(),mySegmentList.begin(),mySegmentList.end(),std::inserter(result, result.end()));
        if (!result.empty())
        {
            address.push_back(it->first);
            segmentData.push_back(result);
        }
    }
}



uint64_t UDPBasicP2P::searchBestSegment(const uint64_t & address)
{

    SegmentMap::iterator it = segmentMap.find(address);
    if (it == segmentMap.end())
        opp_error("Node not found in segmentMap");
    SegmentList * nodeSegmentList = it->second;
    SegmentList result;
    std::set_difference(mySegmentList.begin(),mySegmentList.end(),nodeSegmentList->begin(),nodeSegmentList->end(),std::inserter(result, result.end()));
    if (result.empty())
        opp_error("request error");
    uint16_t min = 64000;
    uint64_t seg = UINT64_MAX;
    for (SegmentList::iterator it2 = result.begin(); it2 != result.end(); ++it2)
    {
        SegmentList::iterator it3 =  mySegmentList.find(*it2);
        if (it3 == mySegmentList.end())
        {
            opp_error("request error segment not found in my list");
            continue;
        }
        if (request[*it2]<min)
        {
            min = request[*it2];
            seg = *it2;
        }
    }
    if (seg == UINT64_MAX)
        opp_error("request error segment not found");

    request[seg]++;
    return seg;
}


void UDPBasicP2P::processRequest(cPacket *p)
{
    UDPBasicPacketP2P *pkt = dynamic_cast<UDPBasicPacketP2P*>(p);
    ClientList::iterator it = clientList.find(pkt->getNodeId().getMAC().getInt());
    if (it == clientList.end())
    {

        InfoClient infoC;
        infoC.numRequestRec = 1;
        infoC.numRequest = 0;
        clientList.insert(std::pair<uint64_t,InfoClient>(pkt->getNodeId().getMAC().getInt(),infoC));
    }
    else
        it->second.numRequestRec++;

    if (pkt->getSubSegmentRequestArraySize() == 0)
    {
        // first request
        UDPBasicPacketP2P *pkt2 = new UDPBasicPacketP2P;
        pkt2->setType(SEGMEN);
        pkt2->setNodeId(myAddress);
        pkt2->setByteLength(maxPacketSize);
        pkt2->setDestination(pkt->getNodeId());
        uint64_t desAdd = pkt->getNodeId().getMAC().getInt();
        pkt2->setSegmentId(searchBestSegment(desAdd));
        uint64_t remain = segmentSize;
        pkt2->setTotalSize(remain);
        uint16_t subSegId = 1;

        if (remain <= maxPacketSize)
        {
            pkt2->setSubSegmentId(0);
            pkt2->setByteLength(remain);
            sendRateLimit(pkt2, desAdd, rateLimit);
            remain = 0;
        }

        while (remain > 0)
        {
            pkt2->setSubSegmentId(subSegId);
            subSegId++;
            if (remain > maxPacketSize)
            {
                sendRateLimit(pkt2->dup(), desAdd, rateLimit);
                remain -= maxPacketSize;
            }
            else
            {
                pkt2->setByteLength(remain);
                sendRateLimit(pkt2, desAdd, rateLimit);
                remain = 0;
            }
        }
    }
    else
    {
        // first request
          UDPBasicPacketP2P *pkt2 = new UDPBasicPacketP2P;
          pkt2->setType(SEGMEN);
          pkt2->setNodeId(myAddress);
          pkt2->setByteLength(maxPacketSize);
          pkt2->setDestination(pkt->getNodeId());
          uint64_t desAdd = pkt->getNodeId().getMAC().getInt();
          pkt2->setSegmentId(pkt->getSegmentId());
          uint64_t remain = (uint64_t)totalSegments * (uint64_t)segmentSize;

          pkt2->setTotalSize(remain);
          for (unsigned int i = 0; i< pkt->getSubSegmentRequestArraySize()-1; i++)
          {
              pkt2->setSubSegmentId(pkt->getSubSegmentRequest(i));
              sendRateLimit(pkt2->dup(), desAdd, rateLimit);
          }
          pkt2->setSubSegmentId(pkt->getSubSegmentRequest(pkt->getSubSegmentRequestArraySize()-1));
          sendRateLimit(pkt2, desAdd, rateLimit);
    }
}

void UDPBasicP2P::sendRateLimit(UDPBasicPacketP2P *pkt, const uint64_t &addr, double rate)
{

    ClientList::iterator it = clientList.find(addr);

    InverseAddres::iterator it2 = inverseAddress.find(addr);
    if (it2 == inverseAddress.end())
        throw cRuntimeError("arp->getInverseAddressResolution address not found %s", MACAddress(addr).str().c_str());
    IPv4Address desAdd = it2->second;

    IPvXAddress auxAddress = IPvXAddress(desAdd);
    if (timeQueue.empty() && simTime() - lastServer > serverTimer)
    {
        // send immediately
        numSent++;
        pkt->setDestination(ManetAddress(MACAddress(addr)));
        if (routing)
        {
            std::deque<MACAddress> pathNode;
            routing->findRoute(par("coverageArea").doubleValue(), MACAddress(addr),pathNode);
            InverseAddres::iterator it = inverseAddress.find(pathNode[0].getInt());
            if (it == inverseAddress.end())
                throw cRuntimeError(" address not found %s", pathNode[0].str().c_str());
            auxAddress = it->second;
        }
        if (pkt->getControlInfo())
            delete pkt->removeControlInfo();
        emit(sentPkSignalP2p, pkt);
        socket.sendTo(pkt, auxAddress, destPort);
        lastPacket = simTime();
        lastServer = simTime();
        it->second.lastMessage = simTime();
    }
    else
    {
        // prepare a delayed
        DelayMessage *delayM = new DelayMessage();
        delayM->setPkt(pkt);
        delayM->setDestination(ManetAddress(MACAddress(addr)));
        //
        it->second.lastMessage += rate;
        lastPacket += rate;
        timeQueue.insert (std::pair<simtime_t,DelayMessage*>(it->second.lastMessage,delayM));
        if (!queueTimer->isScheduled()) //
        {
            if (timeQueue.size() != 1)
                opp_error("Error in timer");
            double dif =  SIMTIME_DBL(simTime() - lastServer);
            scheduleAt(simTime()+ (serverTimer - dif),queueTimer);
        }
        GlobalWirelessLinkInspector::setQueueSize(myAddress, timeQueue.size());
    }
}


void UDPBasicP2P::finish()
{
    recordScalar("packets send", numSent);
    recordScalar("packets received", numReceived);
    recordScalar("segment present", numSegPresent);


    recordScalar("Request sent", numRequestSent);
    recordScalar("request received", numRequestServed);
    recordScalar("segment received", numRequestSegmentServed);

    if (mySegmentList.size() < totalSegments)
    {
        recordScalar("time need", SimTime::getMaxTime());
    }
    else
    {
        double timeNeed = endReception.dbl()-startReception.dbl();
        recordScalar("time need", timeNeed);
    }
}


void UDPBasicP2P::WirelessNumNeig()
{
    // TODO Auto-generated constructor stub
    // fill in routing tables with static routes

    if (!vectorList.empty())
        return;

    cTopology topo("topo");
    topo.extractByProperty("node");
    for (int i = 0; i < topo.getNumNodes(); i++)
    {
        cTopology::Node *destNode = topo.getNode(i);
        IMobility *mod;
        IInterfaceTable* itable = IPvXAddressResolver().findInterfaceTableOf(destNode->getModule());
        bool notfound = true;
        uint64_t add;

        for (int j = 0 ; j < itable->getNumInterfaces(); j++)
        {
            InterfaceEntry *e = itable->getInterface(j);
            if (e->getMacAddress().isUnspecified())
                continue;
            if (e->isLoopback())
                continue;
            if (!notfound)
                break;
            for(SegmentMap::iterator it = segmentMap.begin(); it != segmentMap.end();++it)
            {
                if (it->first == e->getMacAddress().getInt())
                {
                    notfound = false;
                    add = e->getMacAddress().getInt();
                    break;
                }
            }
        }

        if (notfound)
            continue;

        mod = MobilityAccess().get(destNode->getModule());
        if (mod == NULL)
            opp_error("node or mobility module not found");

        vectorList[add] = mod;
    }
}

int UDPBasicP2P::getNumNeighNodes(uint64_t add,double dist)
{
    VectorList::iterator it = vectorList.find(add);
    if (it == vectorList.end())
        opp_error("Node not found");
    int cont = 0;
    Coord ci = it->second->getCurrentPosition();
    for (VectorList::iterator it2 = vectorList.begin();it2 != vectorList.end();++it2)
    {
        if (it2->first == add)
            continue;
        Coord cj = it2->second->getCurrentPosition();
        if (ci.distance(cj) <= dist)
            cont++;
    }
    return cont;
}


void UDPBasicP2P::initializeBurst(int stage)
{
    if (stage == 0)
    {
        counter = 0;
        numSent = 0;
        numReceived = 0;
        numDeleted = 0;
        numDuplicated = 0;

        delayLimit = par("delayLimit");
        startTime = par("startTimeBurst");
        stopTime = par("stopTime");
        if (stopTime >= SIMTIME_ZERO && stopTime <= startTime)
            error("Invalid startTime/stopTime parameters");

        messageLengthPar = &par("messageLength");
        burstDurationPar = &par("burstDuration");
        sleepDurationPar = &par("sleepDuration");
        sendIntervalPar = &par("sendInterval");
        nextSleep = startTime;
        nextBurst = startTime;
        nextPkt = startTime;

        destAddrRNG = par("destAddrRNG");
        const char *addrModeStr = par("chooseDestAddrMode").stringValue();
        int addrMode = cEnum::get("ChooseDestAddrMode")->lookup(addrModeStr);
        if (addrMode == -1)
            throw cRuntimeError("Invalid chooseDestAddrMode: '%s'", addrModeStr);
        chooseDestAddrMode = (ChooseDestAddrMode)addrMode;

        WATCH(numSent);
        WATCH(numReceived);
        WATCH(numDeleted);
        WATCH(numDuplicated);

        localPort = par("localPort");
        destPort = par("destPort");

        timerNext = new cMessage("UDPBasicBurstTimer");
    }
    else if (stage == 5)
        processStart();
}

IPvXAddress UDPBasicP2P::chooseDestAddrBurst()
{
    if (destAddresses.size() == 1)
        return destAddresses[0];

    int k = genk_intrand(destAddrRNG, destAddresses.size());
    return destAddresses[k];
}

cPacket *UDPBasicP2P::createPacket()
{
    char msgName[32];
    sprintf(msgName, "UDPBasicAppData-%d", counter++);
    long msgByteLength = messageLengthPar->longValue();
    cPacket *payload = new cPacket(msgName);
    payload->setByteLength(msgByteLength);
    payload->addPar("sourceId") = getId();
    payload->addPar("msgId") = numSentB;

    return payload;
}

void UDPBasicP2P::processStart()
{
    const char *destAddrs = par("destAddresses");
    cStringTokenizer tokenizer(destAddrs);
    const char *token;
    bool excludeLocalDestAddresses = par("excludeLocalDestAddresses").boolValue();
    if (par("setBroadcast").boolValue())
        socket.setBroadcast(true);

    if (strcmp(par("outputInterface").stringValue(),"") != 0)
    {
        IInterfaceTable* ift = InterfaceTableAccess().get();
        InterfaceEntry *ie = ift->getInterfaceByName(par("outputInterface").stringValue());
        if (ie == NULL)
            throw cRuntimeError(this, "Invalid output interface name : %s",par("outputInterface").stringValue());
        outputInterface = ie->getInterfaceId();
    }

    outputInterfaceMulticastBroadcast.clear();
    if (strcmp(par("outputInterfaceMulticastBroadcast").stringValue(),"") != 0)
    {
        IInterfaceTable* ift = InterfaceTableAccess().get();
        const char *ports = par("outputInterfaceMulticastBroadcast");
        cStringTokenizer tokenizer(ports);
        const char *token;
        while ((token = tokenizer.nextToken()) != NULL)
        {
            if (strstr(token, "ALL") != NULL)
            {
                for (int i = 0; i < ift->getNumInterfaces(); i++)
                {
                    InterfaceEntry *ie = ift->getInterface(i);
                    if (ie->isLoopback())
                        continue;
                    if (ie == NULL)
                        throw cRuntimeError(this, "Invalid output interface name : %s", token);
                    outputInterfaceMulticastBroadcast.push_back(ie->getInterfaceId());
                }
            }
            else
            {
                InterfaceEntry *ie = ift->getInterfaceByName(token);
                if (ie == NULL)
                    throw cRuntimeError(this, "Invalid output interface name : %s", token);
                outputInterfaceMulticastBroadcast.push_back(ie->getInterfaceId());
            }
        }
    }

    IRoutingTable *rt = RoutingTableAccess().getIfExists();

    while ((token = tokenizer.nextToken()) != NULL)
    {
        if (strstr(token, "Broadcast") != NULL)
            destAddresses.push_back(IPv4Address::ALLONES_ADDRESS);
        else
        {
            IPvXAddress addr = IPvXAddressResolver().resolve(token);
            if (excludeLocalDestAddresses && rt && rt->isLocalAddress(addr.get4()))
                continue;
            destAddresses.push_back(addr);
        }
    }

    nextSleep = simTime();
    nextBurst = simTime();
    nextPkt = simTime();
    activeBurst = false;

    isSource = !destAddresses.empty();

    if (isSource)
    {
        if (chooseDestAddrMode == ONCE)
            destAddr = chooseDestAddrBurst();

        activeBurst = true;
    }
    timerNext->setKind(SEND);
    processSend();
}

void UDPBasicP2P::processSend()
{
    if (stopTime < SIMTIME_ZERO || simTime() < stopTime)
    {
        // send and reschedule next sending
        if (isSource) // if the node is a sink, don't generate messages
            generateBurst();
    }
}

void UDPBasicP2P::handleMessageWhenUp(cMessage *msg)
{
    if (msg->getKind() == UDP_I_DATA)
    {
        // process incoming packet
        processPacketBurst(PK(msg));
    }
    else if (msg->getKind() == UDP_I_ERROR)
    {
        EV << "Ignoring UDP error report\n";
        delete msg;
    }
    else
    {
        error("Unrecognized message (%s)%s", msg->getClassName(), msg->getName());
    }

    if (ev.isGUI())
    {
        char buf[40];
        sprintf(buf, "rcvd: %d pks\nsent: %d pks", numReceived, numSent);
        getDisplayString().setTagArg("t", 0, buf);
    }
}

void UDPBasicP2P::processPacketBurst(cPacket *pk)
{
    if (pk->hasPar("sourceId") && pk->hasPar("msgId"))
    {
        // duplicate control
        int moduleId = (int)pk->par("sourceId");
        int msgId = (int)pk->par("msgId");
        SourceSequence::iterator it = sourceSequence.find(moduleId);
        if (it != sourceSequence.end())
        {
            if (it->second >= msgId)
            {
                EV << "Out of order packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
                emit(outOfOrderPkSignal, pk);
                delete pk;
                numDuplicatedB++;
                return;
            }
            else
                it->second = msgId;
        }
        else
            sourceSequence[moduleId] = msgId;
    }

    if (delayLimit > 0)
    {
        if (simTime() - pk->getTimestamp() > delayLimit)
        {
            EV << "Old packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
            emit(dropPkSignal, pk);
            delete pk;
            numDeletedB++;
            return;
        }
    }

    EV << "Received packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
    emit(rcvdPkSignal, pk);
    numReceivedB++;
    delete pk;
}

void UDPBasicP2P::generateBurst()
{
    simtime_t now = simTime();

    if (nextPkt < now)
        nextPkt = now;

    double sendInterval = sendIntervalPar->doubleValue();
    if (sendInterval <= 0.0)
        throw cRuntimeError("The sendInterval parameter must be bigger than 0");
    nextPkt += sendInterval;

    if (activeBurst && nextBurst <= now) // new burst
    {
        double burstDuration = burstDurationPar->doubleValue();
        if (burstDuration < 0.0)
            throw cRuntimeError("The burstDuration parameter mustn't be smaller than 0");
        double sleepDuration = sleepDurationPar->doubleValue();

        if (burstDuration == 0.0)
            activeBurst = false;
        else
        {
            if (sleepDuration < 0.0)
                throw cRuntimeError("The sleepDuration parameter mustn't be smaller than 0");
            nextSleep = now + burstDuration;
            nextBurst = nextSleep + sleepDuration;
        }

        if (chooseDestAddrMode == PER_BURST)
            destAddr = chooseDestAddrBurst();
    }

    if (chooseDestAddrMode == PER_SEND)
        destAddr = chooseDestAddrBurst();

    cPacket *payload = createPacket();
    payload->setTimestamp();
    emit(sentPkSignal, payload);

    UDPBasicPacketP2P *aux = new UDPBasicPacketP2P();
    aux->setType(GENERAL);


    UDPBasicP2P::DirectAddres::iterator it = directAddress.find(destAddr.get4());
    if (it == directAddress.end())
        throw cRuntimeError(" address not found %s", destAddr.get4().str().c_str());

    aux->setDestination(ManetAddress(MACAddress(it->second)));

    numSentB++;
    IPv4Address desAdd = destAddr.get4();
    if (routing)
    {

        std::deque<IPv4Address> pathNode;
        if (routing->findRoute(par("coverageArea").doubleValue(), destAddr.get4(),pathNode))
            desAdd = pathNode[0];
        else
            throw cRuntimeError(" route not found not found %s", destAddr.get4().str().c_str());
    }
    aux->encapsulate(payload);
    socket.sendTo(aux, desAdd, destPort);

    // Next timer
    if (activeBurst && nextPkt >= nextSleep)
        nextPkt = nextBurst;

    if (stopTime >= SIMTIME_ZERO && nextPkt >= stopTime)
    {
        timerNext->setKind(STOP);
        nextPkt = stopTime;
    }
    scheduleAt(nextPkt, timerNext);
}


ManetAddress UDPBasicP2P::DelayMessage::getNodeId() const
{
    return nodeId_var;
}

void UDPBasicP2P::DelayMessage::setNodeId(ManetAddress nodeId)
{
    this->nodeId_var = nodeId;
}

ManetAddress UDPBasicP2P::DelayMessage::getDestination() const
{
    return destination_var;
}

void UDPBasicP2P::DelayMessage::setDestination(ManetAddress destination)
{
    this->destination_var = destination;
}

int UDPBasicP2P::DelayMessage::getType() const
{
    return type_var;
}

void UDPBasicP2P::DelayMessage::setType(int type)
{
    this->type_var = type;
}

int64_t UDPBasicP2P::DelayMessage::getSegmentRequest() const
{
    return segmentRequest_var;
}

void UDPBasicP2P::DelayMessage::setSegmentRequest(int64_t segmentRequest)
{
    this->segmentRequest_var = segmentRequest;
}

uint64_t UDPBasicP2P::DelayMessage::getTotalSize() const
{
    return totalSize_var;
}

void UDPBasicP2P::DelayMessage::setTotalSize(uint64_t totalSize)
{
    this->totalSize_var = totalSize;
}

uint64_t UDPBasicP2P::DelayMessage::getSegmentId() const
{
    return segmentId_var;
}

void UDPBasicP2P::DelayMessage::setSegmentId(uint64_t segmentId)
{
    this->segmentId_var = segmentId;
}

uint16_t UDPBasicP2P::DelayMessage::getSubSegmentId() const
{
    return subSegmentId_var;
}

void UDPBasicP2P::DelayMessage::setSubSegmentId(uint16_t subSegmentId)
{
    this->subSegmentId_var = subSegmentId;
}

UDPBasicPacketP2P * UDPBasicP2P::DelayMessage::getPkt()
{
    UDPBasicPacketP2P *pkt = new UDPBasicPacketP2P();
    pkt->setNodeId(getNodeId());
    pkt->setDestination(getDestination());
    pkt->setType(getType());
    pkt->setSegmentRequest(getSegmentRequest());
    pkt->setTotalSize(getTotalSize());
    pkt->setSegmentId(getSegmentId());
    pkt->setSubSegmentId(getSubSegmentId());
    pkt->setByteLength(getPksize());

    pkt->setSubSegmentRequestArraySize(subSegmentRequest.size());
    for (unsigned int i = 0; i < subSegmentRequest.size();i++)
        pkt->setSubSegmentRequest(i, subSegmentRequest[i]);
    return pkt;
}


void UDPBasicP2P::DelayMessage::setPkt(UDPBasicPacketP2P *pkt)
{
    setNodeId(pkt->getNodeId());
    setDestination(pkt->getDestination());
    setType(pkt->getType());
    setSegmentRequest(pkt->getSegmentRequest());
    setTotalSize(pkt->getTotalSize());
    setSegmentId(pkt->getSegmentId());
    setSubSegmentId(pkt->getSubSegmentId());
    setPksize(pkt->getByteLength());

    for (unsigned int i = 0; i < pkt->getSubSegmentRequestArraySize();i++)
        subSegmentRequest.push_back(pkt->getSubSegmentRequest(i));
    delete pkt;
}
