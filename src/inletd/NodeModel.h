// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Meros Inc.
#pragma once

/*
 * What the NMOS node needs from the rest of inletd: the receiver and sender
 * slots, their streams, and the map from received channels to the device's
 * inputs. The stream manager implements it; the tests implement it with a
 * fake. Slots are 1-based, as they are labelled ("Net In 1").
 */

#include <cstdint>
#include <string>
#include <vector>

#include <aes67/sdp.h>

namespace inlet
{

struct ReceiverInfo
{
    int index = 0;
    std::string label;          ///< "Net In 1"
    int maxChannels = 8;        ///< the most this slot can take
    bool active = false;
    std::string senderId;       ///< what a controller named, when it named one
    std::string sdpText;        ///< what it plays, when active
    aes67_sdp_t sdp {};
    int channels = 0;           ///< what it plays carries, when active
};

struct SenderInfo
{
    int index = 0;
    std::string label;          ///< "Net Out 1"; also the SDP session name
    int channels = 8;
    std::string group;          ///< destination multicast group
    uint16_t port = 5004;
    bool enabled = true;        ///< master_enable
    bool sending = false;       ///< open and on the wire
    std::vector<std::string> channelLabels;   ///< what feeds each channel, as named
};

/** One device input: which received channel feeds it. Receiver 0 = none. */
struct InputMapCell
{
    int receiver = 0;
    int channel = 0;
};

class NodeModel
{
public:
    virtual ~NodeModel() = default;

    virtual std::vector<ReceiverInfo> receivers() const = 0;
    virtual std::vector<SenderInfo> senders() const = 0;
    /** The SDP a sender publishes; empty when it is not sending. */
    virtual std::string senderSdp(int index) const = 0;

    virtual bool subscribeReceiver(int index, const std::string& sdp, const std::string& senderId,
                                   std::string& error) = 0;
    virtual void unsubscribeReceiver(int index) = 0;
    virtual bool setSenderEnabled(int index, bool enabled, std::string& error) = 0;

    /** The device's inputs (what a DAW records), their names, and which
        received channel feeds each (IS-08's active map). */
    virtual std::vector<std::string> deviceInputLabels() const = 0;
    virtual std::vector<InputMapCell> inputMap() const = 0;
    /** All or nothing: (device input, cell) pairs. */
    virtual bool setInputMap(const std::vector<std::pair<int, InputMapCell>>& changes, std::string& error) = 0;

    /** Senders other NMOS nodes publish, found by crawling them: offered in
        the app's stream list. */
    virtual void upsertNmosStream(const std::string& senderId, const std::string& name, const std::string& sdp,
                                  const std::vector<std::string>& channelLabels) = 0;
    virtual void expireNmosStreamsExcept(const std::vector<std::string>& keepSenderIds) = 0;
};

} // namespace inlet
