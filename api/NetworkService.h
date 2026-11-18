// Network management: one cross-platform view of this machine's network - the
// adapters it has and what they are set to, the ports standing open on them,
// the wifi around it, and the hands that change those settings. Windows answers
// through its own calls (iphlpapi, netsh, netcon) and Linux through the ones
// that belong to it (sysfs, /proc, netplan, iw/nmcli, hostapd) - the page in
// front reads one document either way.
//
// Everything the page reads flows through snapshotJson(): a document composed
// fresh each read from pure reads and short-cached questions, so a two-second
// poll costs the machine no more than a few milliseconds. Slow things (a wifi
// scan, a netplan apply, a sharing change) answer on their own doors and say
// what the machine's own tools answered, in their own words.
#ifndef OLS_NETWORKSERVICE_H
#define OLS_NETWORKSERVICE_H

#include <string>

namespace ols
{
  namespace net
  {

    // Start the background sampler that keeps the bandwidth history: called
    // once at server start, before the doors answer. Safe to call before the
    // listener opens.
    void init();

    // The whole network document: platform, elevation, uptime, features (what
    // this kind of machine can do from here and why not otherwise), adapters
    // with settings and counters and rate history, wifi state, access-point
    // state, sharing state, and the ports in use.
    std::string snapshotJson();

    // The wifi scan table. A plain read answers from the kept scan (scans are
    // asked of the radio and cost real seconds); force re-asks the radio now.
    std::string scanJson(bool force);

    // The work door. The body names an action and its parts:
    //   adapter_up / adapter_down      {name}
    //   mtu_set                        {name, mtu}
    //   ip_set                         {name, mode: dhcp|static, address,
    //                                   prefix, gateway, dns[], metric}
    //   wifi_connect                   {interface?, ssid, psk?, auth?, hidden?}
    //   wifi_disconnect                {interface?}
    //   wifi_forget                    {ssid, interface?}
    //   radio_block                    {interface?, on}
    //   bond_create                    {name, slaves[], mode, primary?,
    //                                   address?, prefix?, gateway?, dns[], metric?}
    //   bond_delete                    {name}
    //   vlan_create                    {name?, parent, id, address?, prefix?,
    //                                   gateway?, dns[], metric?}
    //   vlan_delete                    {name}
    //   share_enable                   {external, internal}
    //   share_disable                  {}
    //   ap_start                       {interface?, ssid, psk?, band?, channel?,
    //                                   country?, max_clients?, beacon_ms?}
    //   ap_stop                        {}
    // The answer is always json: at least {did, words, action}.
    std::string actJson(const std::string &bodyJson);

  } // namespace net
} // namespace ols

#endif // OLS_NETWORKSERVICE_H
