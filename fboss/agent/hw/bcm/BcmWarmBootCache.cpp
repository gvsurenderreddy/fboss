/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "BcmWarmBootCache.h"
#include <limits>
#include <string>
#include <utility>

#include <folly/Conv.h>
#include <folly/FileUtil.h>
#include <folly/dynamic.h>
#include <folly/json.h>

#include "fboss/agent/Constants.h"
#include "fboss/agent/hw/bcm/BcmEgress.h"
#include "fboss/agent/hw/bcm/BcmPlatform.h"
#include "fboss/agent/hw/bcm/BcmSwitch.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/Utils.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/ArpTable.h"
#include "fboss/agent/state/NdpTable.h"
#include "fboss/agent/state/NeighborEntry.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/SysError.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/VlanMap.h"
#include "fboss/agent/state/SwitchState.h"

using std::make_pair;
using std::make_tuple;
using std::make_shared;
using std::numeric_limits;
using std::string;
using std::vector;
using std::shared_ptr;
using boost::container::flat_map;
using folly::ByteRange;
using folly::IPAddress;
using folly::MacAddress;
using boost::container::flat_map;
using boost::container::flat_set;
using namespace facebook::fboss;

namespace {
auto constexpr kEcmpObjects = "ecmpObjects";
auto constexpr kVlanForCPUEgressEntries = 0;

struct AddrTables {
  AddrTables() : arpTable(make_shared<ArpTable>()),
      ndpTable(make_shared<NdpTable>()) {}
  shared_ptr<facebook::fboss::ArpTable> arpTable;
  shared_ptr<facebook::fboss::NdpTable> ndpTable;
};

folly::IPAddress getFullMaskIPv4Address() {
  return folly::IPAddress(folly::IPAddressV4(
      folly::IPAddressV4::fetchMask(folly::IPAddressV4::bitCount())));
}

folly::IPAddress getFullMaskIPv6Address() {
  return folly::IPAddress(folly::IPAddressV6(
      folly::IPAddressV6::fetchMask(folly::IPAddressV6::bitCount())));
}
}

namespace facebook { namespace fboss {

BcmWarmBootCache::BcmWarmBootCache(const BcmSwitch* hw)
    : hw_(hw),
      dropEgressId_(BcmEgressBase::INVALID),
      toCPUEgressId_(BcmEgressBase::INVALID) {}

shared_ptr<InterfaceMap> BcmWarmBootCache::reconstructInterfaceMap() const {
  std::shared_ptr<InterfaceMap> dumpedInterfaceMap =
      dumpedSwSwitchState_->getInterfaces();
  auto intfMap = make_shared<InterfaceMap>();
  for (const auto& vlanMacAndIntf: vlanAndMac2Intf_) {
    const auto& bcmIntf = vlanMacAndIntf.second;
    std::shared_ptr<Interface> dumpedInterface =
        dumpedInterfaceMap->getInterfaceIf(InterfaceID(bcmIntf.l3a_vid));
    std::string dumpedInterfaceName = dumpedInterface->getName();
    auto newInterface = make_shared<Interface>(InterfaceID(bcmIntf.l3a_vid),
                                               RouterID(bcmIntf.l3a_vrf),
                                               VlanID(bcmIntf.l3a_vid),
                                               dumpedInterfaceName,
                                               vlanMacAndIntf.first.second,
                                               bcmIntf.l3a_mtu);
    newInterface->setAddresses(dumpedInterface->getAddresses());
    intfMap->addInterface(newInterface);
  }
  return intfMap;
}

shared_ptr<VlanMap> BcmWarmBootCache::reconstructVlanMap() const {
  std::shared_ptr<VlanMap> dumpedVlans = dumpedSwSwitchState_->getVlans();
  auto vlans = make_shared<VlanMap>();
  flat_map<VlanID, VlanFields> vlan2VlanFields;
  // Get vlan and port mapping
  for (auto vlanAndInfo: vlan2VlanInfo_) {
    // Note : missing vlan name. This should be
    // fixed with t4155406
    auto vlan = make_shared<Vlan>(vlanAndInfo.first, "");
    flat_set<int> untagged;
    int idx;
    OPENNSL_PBMP_ITER(vlanAndInfo.second.untagged, idx) {
      vlan->addPort(PortID(idx), false);
      untagged.insert(idx);
    }
    OPENNSL_PBMP_ITER(vlanAndInfo.second.allPorts, idx) {
      if (untagged.find(idx) != untagged.end()) {
        continue;
      }
      vlan->addPort(PortID(idx), true);
    }
    vlan->setInterfaceID(vlanAndInfo.second.intfID);
    vlans->addVlan(vlan);
  }
  flat_map<VlanID, AddrTables> vlan2AddrTables;
  // Populate ARP and NDP tables of VLANs using egress entries
  for (auto vrfIpAndHost : vrfIp2Host_) {
    const auto& vrf = vrfIpAndHost.first.first;
    const auto& ip = vrfIpAndHost.first.second;
    const auto egressIdAndEgressBool = findEgress(vrf, ip);
    if (egressIdAndEgressBool == egressId2EgressAndBool_.end()) {
      // The host entry might be an ECMP egress entry.
      continue;
    }
    const auto& bcmEgress = egressIdAndEgressBool->second.first;
    if (bcmEgress.vlan == kVlanForCPUEgressEntries) {
      // Ignore to CPU egress entries which get mapped to vlan 0
      continue;
    }
    const auto& vlanId = VlanID(bcmEgress.vlan);
    // Is this ip a route or interface? If this is an entry for a route, then we
    // don't want to add this to the warm boot state.
    if (dumpedVlans) {
      const auto& dumpedVlan = dumpedVlans->getVlan(vlanId);
      if (ip.isV4() && !dumpedVlan->getArpTable()->getEntryIf(ip.asV4())) {
        continue;     // to next host entry
      }
      if (ip.isV6() && !dumpedVlan->getNdpTable()->getEntryIf(ip.asV6())) {
        continue;     // to next host entry
      }
    }
    auto titr = vlan2AddrTables.find(vlanId);
    if (titr == vlan2AddrTables.end()) {
      titr = vlan2AddrTables.insert(make_pair(vlanId, AddrTables())).first;
    }

    // If we have a drop entry programmed for an existing host, it is a
    // pending entry
    if (ip.isV4()) {
      auto arpTable = titr->second.arpTable;
      if (BcmEgress::programmedToDrop(bcmEgress)) {
        arpTable->addPendingEntry(ip.asV4(), InterfaceID(bcmEgress.vlan));
      } else {
        arpTable->addEntry(ip.asV4(), macFromBcm(bcmEgress.mac_addr),
                           PortID(bcmEgress.port), InterfaceID(bcmEgress.vlan),
                           NeighborState::UNVERIFIED);
      }
    } else {
      auto ndpTable = titr->second.ndpTable;
      if (BcmEgress::programmedToDrop(bcmEgress)) {
        ndpTable->addPendingEntry(ip.asV6(), InterfaceID(bcmEgress.vlan));
      } else {
        ndpTable->addEntry(ip.asV6(), macFromBcm(bcmEgress.mac_addr),
                           PortID(bcmEgress.port), InterfaceID(bcmEgress.vlan),
                           NeighborState::UNVERIFIED);
      }
    }
  }
  for (auto vlanAndAddrTable: vlan2AddrTables) {
    auto vlan = vlans->getVlanIf(vlanAndAddrTable.first);
    if(!vlan) {
      LOG(FATAL) << "Vlan: " << vlanAndAddrTable.first <<  " not found";
    }
    vlan->setArpTable(vlanAndAddrTable.second.arpTable);
    vlan->setNdpTable(vlanAndAddrTable.second.ndpTable);
  }
  return vlans;
}


const BcmWarmBootCache::EgressIds&
BcmWarmBootCache::getPathsForEcmp(EgressId ecmp) const {
  CHECK(hwSwitchEcmp2EgressIdsPopulated_);
  static const EgressIds kEmptyEgressIds;
  if (hwSwitchEcmp2EgressIds_.empty()) {
    // We may have empty hwSwitchEcmp2EgressIds_ when
    // we exited with no ECMP entries.
    return kEmptyEgressIds;
  }
  auto itr = hwSwitchEcmp2EgressIds_.find(ecmp);
  if (itr == hwSwitchEcmp2EgressIds_.end()) {
    throw FbossError("Could not find ecmp ID : ", ecmp);
  }
  return itr->second;
}

folly::dynamic BcmWarmBootCache::toFollyDynamic() const {
  folly::dynamic warmBootCache = folly::dynamic::object;
  // For now we serialize only the hwSwitchEcmp2EgressIds_ table.
  // This is the only thing we need and may not be able to get
  // from HW in the case where we shut down before doing a FIB sync.
  std::vector<folly::dynamic> ecmps;
  for (auto& ecmpAndEgressIds : hwSwitchEcmp2EgressIds_) {
    folly::dynamic ecmp = folly::dynamic::object;
    ecmp[kEcmpEgressId] = ecmpAndEgressIds.first;
    std::vector<folly::dynamic> paths;
    for (auto path : ecmpAndEgressIds.second) {
      paths.emplace_back(path);
    }
    ecmp[kPaths] = std::move(paths);
    ecmps.emplace_back(std::move(ecmp));
  }
  warmBootCache[kEcmpObjects] = std::move(ecmps);
  return warmBootCache;
}

void BcmWarmBootCache::populateStateFromWarmbootFile() {
  string warmBootJson;
  const auto& warmBootFile = hw_->getPlatform()->getWarmBootSwitchStateFile();
  auto ret = folly::readFile(warmBootFile.c_str(), warmBootJson);
  sysCheckError(ret, "Unable to read switch state from : ", warmBootFile);
  auto switchStateJson = folly::parseJson(warmBootJson);
  if (switchStateJson.find(kSwSwitch) != switchStateJson.items().end()) {
    dumpedSwSwitchState_ =
        SwitchState::uniquePtrFromFollyDynamic(switchStateJson[kSwSwitch]);
  } else {
    dumpedSwSwitchState_ =
        SwitchState::uniquePtrFromFollyDynamic(switchStateJson);
  }
  CHECK(dumpedSwSwitchState_)
      << "Was not able to recover software state after warmboot from state "
         "file: " << hw_->getPlatform()->getWarmBootSwitchStateFile();

  if (switchStateJson.find(kHwSwitch) == switchStateJson.items().end()) {
    // hwSwitch state does not exist no need to reconstruct
    // ecmp -> egressId map. We only started dumping this
    // when we added fast handling of updating ecmp entries
    // on link down. So on update from a version which does
    // not have this fast handling its expected that this
    // JSON wont exist.
    VLOG (1) << "Hw switch state does not exist, skipped reconstructing "
      << "ECMP -> egressIds map ";
    return;
  }
  hwSwitchEcmp2EgressIdsPopulated_ = true;
  // Extract ecmps for dumped host table
  auto hostTable = switchStateJson[kHwSwitch][kHostTable];
  for (const auto& ecmpEntry : hostTable[kEcmpHosts]) {
    auto ecmpEgressId = ecmpEntry[kEcmpEgressId].asInt();
    if (ecmpEgressId == BcmEgressBase::INVALID) {
      continue;
    }
    // If the entry is valid, then there must be paths associated with it.
    for (auto path : ecmpEntry[kEcmpEgress][kPaths]) {
      hwSwitchEcmp2EgressIds_[ecmpEgressId].insert(path.asInt());
    }
  }
  // Extract ecmps from dumped warm boot cache. We
  // may have shut down before a FIB sync
  auto ecmpObjects = switchStateJson[kHwSwitch][kWarmBootCache][kEcmpObjects];
  for (const auto& ecmpEntry : ecmpObjects) {
    auto ecmpEgressId = ecmpEntry[kEcmpEgressId].asInt();
    CHECK(ecmpEgressId != BcmEgressBase::INVALID);
    for (auto path : ecmpEntry[kPaths]) {
      hwSwitchEcmp2EgressIds_[ecmpEgressId].emplace(path.asInt());
    }
  }
  VLOG (1) << "Reconstructed following ecmp path map ";
  for (auto& ecmpIdAndEgress : hwSwitchEcmp2EgressIds_) {
    VLOG(1) << ecmpIdAndEgress.first << " (from warmboot file) ==> "
            << toEgressIdsStr(ecmpIdAndEgress.second);
  }
}

void BcmWarmBootCache::populate() {
  populateStateFromWarmbootFile();
  opennsl_vlan_data_t* vlanList = nullptr;
  int vlanCount = 0;
  SCOPE_EXIT {
    opennsl_vlan_list_destroy(hw_->getUnit(), vlanList, vlanCount);
  };
  auto rv = opennsl_vlan_list(hw_->getUnit(), &vlanList, &vlanCount);
  bcmCheckError(rv, "Unable to get vlan information");
  for (auto i = 0; i < vlanCount; ++i) {
    opennsl_vlan_data_t& vlanData = vlanList[i];
    int portCount;
    OPENNSL_PBMP_COUNT(vlanData.port_bitmap, portCount);
    VLOG (1) << "Got vlan : " << vlanData.vlan_tag
      <<" with : " << portCount << " ports";
    // TODO: Investigate why port_bitmap contains
    // the untagged ports rather than ut_port_bitmap
    vlan2VlanInfo_.insert(make_pair
        (BcmSwitch::getVlanId(vlanData.vlan_tag),
         VlanInfo(VlanID(vlanData.vlan_tag), vlanData.port_bitmap,
           vlanData.port_bitmap)));
    opennsl_l3_intf_t l3Intf;
    opennsl_l3_intf_t_init(&l3Intf);
    // Implicit here is the assumption that we have a interface
    // per vlan (since we are looking up the inteface by vlan).
    // If this changes we will have to store extra information
    // somewhere (e.g. interface id or vlan, mac pairs for interfaces
    // created) and then use that for lookup during warm boot.
    l3Intf.l3a_vid = vlanData.vlan_tag;
    bool intfFound = false;
    rv = opennsl_l3_intf_find_vlan(hw_->getUnit(), &l3Intf);
    if (rv != OPENNSL_E_NOT_FOUND) {
      bcmCheckError(rv, "failed to find interface for ",
          vlanData.vlan_tag);
      intfFound = true;
      vlanAndMac2Intf_[make_pair(BcmSwitch::getVlanId(l3Intf.l3a_vid),
          macFromBcm(l3Intf.l3a_mac_addr))] = l3Intf;
      VLOG(1) << "Found l3 interface for vlan : " << vlanData.vlan_tag;
    }
    if (intfFound) {
      opennsl_l2_station_t l2Station;
      opennsl_l2_station_t_init(&l2Station);
      rv = opennsl_l2_station_get(hw_->getUnit(), l3Intf.l3a_vid, &l2Station);
      if (!OPENNSL_FAILURE(rv)) {
        VLOG (1) << " Found l2 station with id : " << l3Intf.l3a_vid;
        vlan2Station_[VlanID(vlanData.vlan_tag)] = l2Station;
      } else {
        // FIXME Why are we unable to find l2 stations on a warm boot ?.
        VLOG(1) << "Could not get l2 station for vlan : " << vlanData.vlan_tag;
      }
    }
  }
  opennsl_l3_info_t l3Info;
  opennsl_l3_info_t_init(&l3Info);
  opennsl_l3_info(hw_->getUnit(), &l3Info);
  // Traverse V4 hosts
  opennsl_l3_host_traverse(hw_->getUnit(), 0, 0, l3Info.l3info_max_host,
      hostTraversalCallback, this);
  // Traverse V6 hosts
  opennsl_l3_host_traverse(hw_->getUnit(), OPENNSL_L3_IP6, 0,
      // Diag shell uses this for getting # of v6 host entries
      l3Info.l3info_max_host / 2,
      hostTraversalCallback, this);
  // Traverse V4 routes
  opennsl_l3_route_traverse(hw_->getUnit(), 0, 0, l3Info.l3info_max_route,
      routeTraversalCallback, this);
  // Traverse V6 routes
  opennsl_l3_route_traverse(hw_->getUnit(), OPENNSL_L3_IP6, 0,
      // Diag shell uses this for getting # of v6 route entries
      l3Info.l3info_max_route / 2,
      routeTraversalCallback, this);
  // Get egress entries. This is done after we have traversed through host and
  // route entries, so we have populated egressOrEcmpIdsFromHostTable_.
  opennsl_l3_egress_traverse(hw_->getUnit(), egressTraversalCallback, this);
  // Traverse ecmp egress entries
  opennsl_l3_egress_ecmp_traverse(hw_->getUnit(), ecmpEgressTraversalCallback,
      this);

  // Clear the egresses that were collected during populate() to find out
  // egress ids corresponding to drop egress and cpu egress.
  egressOrEcmpIdsFromHostTable_.clear();
}

bool BcmWarmBootCache::fillVlanPortInfo(Vlan* vlan) {
  auto vlanItr = vlan2VlanInfo_.find(vlan->getID());
  if (vlanItr != vlan2VlanInfo_.end()) {
    Vlan::MemberPorts memberPorts;
    opennsl_port_t idx;
    OPENNSL_PBMP_ITER(vlanItr->second.untagged, idx) {
      memberPorts.insert(make_pair(PortID(idx), false));
    }
    OPENNSL_PBMP_ITER(vlanItr->second.allPorts, idx) {
      if (memberPorts.find(PortID(idx)) == memberPorts.end()) {
        memberPorts.insert(make_pair(PortID(idx), true));
      }
    }
    vlan->setPorts(memberPorts);
    return true;
  }
  return false;
}

int BcmWarmBootCache::hostTraversalCallback(int unit, int index,
    opennsl_l3_host_t* host, void* userData) {
  BcmWarmBootCache* cache = static_cast<BcmWarmBootCache*>(userData);
  auto ip = host->l3a_flags & OPENNSL_L3_IP6 ?
    IPAddress::fromBinary(ByteRange(host->l3a_ip6_addr,
          sizeof(host->l3a_ip6_addr))) :
    IPAddress::fromLongHBO(host->l3a_ip_addr);
  cache->vrfIp2Host_[make_pair(host->l3a_vrf, ip)] = *host;
  VLOG(1) << "Adding egress id: " << host->l3a_intf << " to " << ip
          << " mapping";
  cache->egressOrEcmpIdsFromHostTable_.insert(host->l3a_intf);
  return 0;
}

int BcmWarmBootCache::egressTraversalCallback(int unit,
                                              EgressId egressId,
                                              opennsl_l3_egress_t* egress,
                                              void* userData) {
  BcmWarmBootCache* cache = static_cast<BcmWarmBootCache*>(userData);
  CHECK(cache->egressId2EgressAndBool_.find(egressId) ==
        cache->egressId2EgressAndBool_.end())
      << "Double callback for egress id: " << egressId;
  // Look up egressId in egressOrEcmpIdsFromHostTable_ and populate either
  // dropEgressId_ or toCPUEgressId_.
  auto egressIdItr = cache->egressOrEcmpIdsFromHostTable_.find(egressId);
  if (egressIdItr != cache->egressOrEcmpIdsFromHostTable_.end()) {
    // May be: Add information to figure out how many host or route entry
    // reference it.
    VLOG(1) << "Adding bcm egress entry for: " << *egressIdItr
            << " which is referenced by at least one host or route entry.";
    cache->egressId2EgressAndBool_[egressId] = std::make_pair(*egress, false);
  } else {
    // found egress ID that is not used by any host entry, we shall
    // only have two of them. One is for drop and the other one is for TO CPU.
    if ((egress->flags & OPENNSL_L3_DST_DISCARD)) {
      if (cache->dropEgressId_ != BcmEgressBase::INVALID) {
        LOG(FATAL) << "duplicated drop egress found in HW. " << egressId
                   << " and " << cache->dropEgressId_;
      }
      VLOG(1) << "Found drop egress id " << egressId;
      cache->dropEgressId_ = egressId;
    } else if ((egress->flags &
                (OPENNSL_L3_L2TOCPU | OPENNSL_L3_COPY_TO_CPU))) {
      if (cache->toCPUEgressId_ != BcmEgressBase::INVALID) {
        LOG(FATAL) << "duplicated generic TO_CPU egress found in HW. "
                   << egressId << " and " << cache->toCPUEgressId_;
      }
      VLOG(1) << "Found generic TO CPU egress id " << egressId;
      cache->toCPUEgressId_ = egressId;
    } else {
      LOG(FATAL) << "The egress: " << egressId
                 << " is not referenced by any host entry. vlan: "
                 << egress->vlan << " interface: " << egress->intf
                 << " flags: " << std::hex << egress->flags << std::dec;
    }
  }
  return 0;
}

int BcmWarmBootCache::routeTraversalCallback(int unit, int index,
    opennsl_l3_route_t* route, void* userData) {
  BcmWarmBootCache* cache = static_cast<BcmWarmBootCache*>(userData);
  bool isIPv6 = route->l3a_flags & OPENNSL_L3_IP6;
  auto ip = isIPv6 ? IPAddress::fromBinary(ByteRange(
                         route->l3a_ip6_net, sizeof(route->l3a_ip6_net)))
                   : IPAddress::fromLongHBO(route->l3a_subnet);
  auto mask = isIPv6 ? IPAddress::fromBinary(ByteRange(
                           route->l3a_ip6_mask, sizeof(route->l3a_ip6_mask)))
                     : IPAddress::fromLongHBO(route->l3a_ip_mask);
  if (cache->getHw()->getPlatform()->canUseHostTableForHostRoutes() &&
      ((isIPv6 && mask == getFullMaskIPv6Address()) ||
       (!isIPv6 && mask == getFullMaskIPv4Address()))) {
    // This is a host route.
    cache->vrfAndIP2Route_[make_pair(route->l3a_vrf, ip)] = *route;
    VLOG(3) << "Adding host route found in route table. vrf: "
            << route->l3a_vrf << " ip: " << ip << " mask: " << mask;
  } else {
    // Other routes that cannot be put into host table / CAM.
    cache->vrfPrefix2Route_[make_tuple(route->l3a_vrf, ip, mask)] = *route;
    VLOG(3) << "In vrf : " << route->l3a_vrf << " adding route for : " << ip
            << " mask: " << mask;
  }
  return 0;
}

int BcmWarmBootCache::ecmpEgressTraversalCallback(int unit,
    opennsl_l3_egress_ecmp_t *ecmp, int intfCount, opennsl_if_t *intfArray,
    void *userData) {
  BcmWarmBootCache* cache = static_cast<BcmWarmBootCache*>(userData);
  EgressIds egressIds;
  if (cache->hwSwitchEcmp2EgressIdsPopulated_) {
    // Rather than using the egressId in the intfArray we use the
    // egressIds that we dumped as part of the warm boot state. IntfArray
    // does not include any egressIds that go over the ports that may be
    // down while the warm boot state we dumped does
    try {
      egressIds = cache->getPathsForEcmp(ecmp->ecmp_intf);
    } catch (const FbossError& ex) {
      // There was a bug in SDK where sometimes we got callback with invalid
      // ecmp id with zero number of interfaces. This happened for double wide
      // ECMP entries (when two "words" are used to represent one ECMP entry).
      // For example, if the entries were 200256 and 200258, we got callback
      // for 200257 also with zero interfaces associated with it. If this is
      // the case, we skip this entry.
      //
      // We can also get intfCount of zero with valid ecmp entry (when all the
      // links associated with egress of the ecmp are down. But in this case,
      // cache->getPathsForEcmp() call above should return a valid set of
      // egressIds.
      if (intfCount == 0) {
        return 0;
      }
      throw ex;
    }
    EgressIds egressIdsInHw;
    egressIdsInHw = cache->toEgressIds(intfArray, intfCount);
    VLOG(1) << "ignoring paths for ecmp egress " << ecmp->ecmp_intf
            << " gotten from hardware: " << toEgressIdsStr(egressIdsInHw);
  } else {
    if (intfCount == 0) {
      return 0;
    }
    egressIds = cache->toEgressIds(intfArray, intfCount);
  }
  CHECK(egressIds.size() > 0)
      << "There must be at least one egress pointed to by the ecmp egress id: "
      << ecmp->ecmp_intf;
  CHECK(cache->egressIds2Ecmp_.find(egressIds) == cache->egressIds2Ecmp_.end())
      << "Got a duplicated call for ecmp id: " << ecmp->ecmp_intf
      << " referencing: " << toEgressIdsStr(egressIds);
  cache->egressIds2Ecmp_[egressIds] = *ecmp;
  VLOG(1) << "Added ecmp egress id : " << ecmp->ecmp_intf
          << " pointing to : " << toEgressIdsStr(egressIds) << " egress ids";
  return 0;
}

std::string BcmWarmBootCache::toEgressIdsStr(const EgressIds& egressIds) {
  string egressStr;
  int i = 0;
  for (auto egressId : egressIds) {
    egressStr += folly::to<string>(egressId);
    egressStr += ++i < egressIds.size() ?  ", " : "";
  }
  return egressStr;
}

void BcmWarmBootCache::clear() {
  // Get rid of all unclaimed entries. The order is important here
  // since we want to delete entries only after there are no more
  // references to them.
  VLOG(1) << "Warm boot: removing unreferenced entries";
  dumpedSwSwitchState_.reset();
  hwSwitchEcmp2EgressIds_.clear();
  // First delete routes (fully qualified and others).
  //
  // Nothing references routes, but routes reference ecmp egress and egress
  // entries which are deleted later
  for (auto vrfPfxAndRoute : vrfPrefix2Route_) {
    VLOG(1) << "Deleting unreferenced route in vrf:" <<
        std::get<0>(vrfPfxAndRoute.first) << " for prefix : " <<
        std::get<1>(vrfPfxAndRoute.first) << "/" <<
        std::get<2>(vrfPfxAndRoute.first);
    auto rv = opennsl_l3_route_delete(hw_->getUnit(), &(vrfPfxAndRoute.second));
    bcmLogFatal(rv, hw_, "failed to delete unreferenced route in vrf:",
        std::get<0>(vrfPfxAndRoute.first) , " for prefix : " ,
        std::get<1>(vrfPfxAndRoute.first) , "/" ,
        std::get<2>(vrfPfxAndRoute.first));
  }
  vrfPrefix2Route_.clear();
  for (auto vrfIPAndRoute: vrfAndIP2Route_) {
    VLOG(1) << "Deleting fully qualified unreferenced route in vrf: "
            << vrfIPAndRoute.first.first
            << " prefix: " << vrfIPAndRoute.first.second;
    auto rv = opennsl_l3_route_delete(hw_->getUnit(), &(vrfIPAndRoute.second));
    bcmLogFatal(rv,
                hw_,
                "failed to delete fully qualified unreferenced route in vrf: ",
                vrfIPAndRoute.first.first,
                " prefix: ",
                vrfIPAndRoute.first.second);
  }
  vrfAndIP2Route_.clear();

  // Delete bcm host entries. Nobody references bcm hosts, but
  // hosts reference egress objects
  for (auto vrfIpAndHost : vrfIp2Host_) {
    VLOG(1) << "Deleting host entry in vrf: " <<
        vrfIpAndHost.first.first << " for : " << vrfIpAndHost.first.second;
    auto rv = opennsl_l3_host_delete(hw_->getUnit(), &vrfIpAndHost.second);
    bcmLogFatal(rv, hw_, "failed to delete host entry in vrf: ",
        vrfIpAndHost.first.first, " for : ", vrfIpAndHost.first.second);
  }
  vrfIp2Host_.clear();

  // Both routes and host entries (which have been deleted earlier) can refer
  // to ecmp egress objects.  Ecmp egress objects in turn refer to egress
  // objects which we delete later
  for (auto idsAndEcmp : egressIds2Ecmp_) {
    auto& ecmp = idsAndEcmp.second;
    VLOG(1) << "Deleting ecmp egress object  " << ecmp.ecmp_intf
      << " pointing to : " << toEgressIdsStr(idsAndEcmp.first);
    auto rv = opennsl_l3_egress_ecmp_destroy(hw_->getUnit(), &ecmp);
    bcmLogFatal(rv, hw_, "failed to destroy ecmp egress object :",
        ecmp.ecmp_intf, " referring to ",
        toEgressIdsStr(idsAndEcmp.first));
  }
  egressIds2Ecmp_.clear();

  // Delete bcm egress entries. These are referenced by routes, ecmp egress
  // and host objects all of which we deleted above. Egress objects in turn
  // my point to a interface which we delete later
  for (auto egressIdAndEgressBool : egressId2EgressAndBool_) {
    if (!egressIdAndEgressBool.second.second) {
      // This is not used yet
      VLOG(1) << "Deleting egress object: " << egressIdAndEgressBool.first;
      auto rv = opennsl_l3_egress_destroy(hw_->getUnit(),
                                          egressIdAndEgressBool.first);
      bcmLogFatal(rv,
                  hw_,
                  "failed to destroy egress object ",
                  egressIdAndEgressBool.first);
    }
  }
  egressId2EgressAndBool_.clear();

  // Delete interfaces
  for (auto vlanMacAndIntf : vlanAndMac2Intf_) {
    VLOG(1) <<"Deletingl3 interface for vlan: " << vlanMacAndIntf.first.first
      <<" and mac : " << vlanMacAndIntf.first.second;
    auto rv = opennsl_l3_intf_delete(hw_->getUnit(), &vlanMacAndIntf.second);
    bcmLogFatal(rv, hw_, "failed to delete l3 interface for vlan: ",
        vlanMacAndIntf.first.first, " and mac : ", vlanMacAndIntf.first.second);
  }
  vlanAndMac2Intf_.clear();
  // Delete stations
  for (auto vlanAndStation : vlan2Station_) {
    VLOG(1) << "Deleting station for vlan : " << vlanAndStation.first;
    auto rv = opennsl_l2_station_delete(hw_->getUnit(), vlanAndStation.first);
    bcmLogFatal(rv, hw_, "failed to delete station for vlan : ",
        vlanAndStation.first);
  }
  vlan2Station_.clear();
  opennsl_vlan_t defaultVlan;
  auto rv = opennsl_vlan_default_get(hw_->getUnit(), &defaultVlan);
  bcmLogFatal(rv, hw_, "failed to get default VLAN");
  // Finally delete the vlans
  for (auto vlanItr = vlan2VlanInfo_.begin();
      vlanItr != vlan2VlanInfo_.end();) {
    if (defaultVlan == vlanItr->first) {
      ++vlanItr;
      continue; // Can't delete the default vlan
    }
    VLOG(1) << "Deleting vlan : " << vlanItr->first;
    auto rv = opennsl_vlan_destroy(hw_->getUnit(), vlanItr->first);
    bcmLogFatal(rv, hw_, "failed to destroy vlan: ", vlanItr->first);
    vlanItr = vlan2VlanInfo_.erase(vlanItr);
  }
}
}}
