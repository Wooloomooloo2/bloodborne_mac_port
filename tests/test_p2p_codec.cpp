// The online module's P2P wire format: the frames shadPS4 v0.20.0's FramingCodec
// (gpu/bbnet/shadps4/core/net/p2p_codec.cpp) puts on the UDP socket, byte for byte, so the mac
// port's clients stay compatible with mainline shadPS4 and the Windows port (which vendors the
// same code). The earlier module (upstream bbport's shadp2p tree) framed every datagram as
// [src vport][dst vport] + payload, which this codec rejects, and could not co-op with them.
// Built by gpu/bbnet/CMakeLists.txt (bbnet-codec-test), run by build_macos.sh --test.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/net/p2p_codec.h"

using namespace Core::Net::P2P;

namespace {
int failures;

std::string Hex(std::span<const u8> bytes) {
    std::string out;
    char buf[4];
    for (const u8 b : bytes) {
        std::snprintf(buf, sizeof(buf), "%02x ", b);
        out += buf;
    }
    return out;
}

void Expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

void ExpectBytes(const std::vector<u8>& got, const std::vector<u8>& want, const char* what) {
    const bool ok = got == want;
    Expect(ok, what);
    if (!ok) {
        std::printf("  got  %s\n  want %s\n", Hex(got).c_str(), Hex(want).c_str());
    }
}

std::vector<u8> Cat(std::vector<u8> head, std::span<const u8> tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}
} // namespace

int main() {
    const Endpoint peer = Endpoint::IPv4("63.142.190.115", 27015);
    auto keyring = std::make_shared<Keyring>();
    FramingCodec codec(keyring);

    // NpSignaling's handshake (SignalingHandshake, 'SHAD' magic) goes to and from vport 0xFFFF.
    const std::vector<u8> handshake{'S', 'H', 'A', 'D', 0x01, 0x00, 0x00, 0x00, 0x2a};
    const auto signaling = codec.EncodeSignaling(handshake, peer);
    ExpectBytes(signaling, Cat({0xFF, 0x83, 0xFF, 0xFF, 0xFF, 0xFF}, handshake),
                "signaling frame: marker, valid|datagram, vport FFFF -> FFFF, payload");
    {
        const auto decoded = codec.Decode(signaling, peer);
        Expect(decoded.kind == Codec::Kind::Signaling && decoded.src_vport == 0xFFFF &&
                   decoded.dst_vport == 0xFFFF && decoded.payload == handshake,
               "signaling frame decodes as signaling with its payload");
    }

    // Game datagrams carry their vports big-endian.
    const std::vector<u8> payload{1, 2, 3, 4, 5};
    const auto datagram = codec.EncodeDatagram(0x04D2, 0x162E, payload, peer, {});
    ExpectBytes(datagram, Cat({0xFF, 0x83, 0x04, 0xD2, 0x16, 0x2E}, payload),
                "datagram frame: vports 1234 -> 5678 big-endian, no digest");
    {
        const auto decoded = codec.Decode(datagram, peer);
        Expect(decoded.kind == Codec::Kind::Datagram && decoded.src_vport == 0x04D2 &&
                   decoded.dst_vport == 0x162E && decoded.payload == payload,
               "datagram frame decodes with its vports");
    }

    // The communication id NP installs (ioctl 0xd8) travels as a 4-byte digest after the vports.
    keyring->SetCommunicationId(CommunicationId{0x11, 0x22, 0x33, 0x44});
    const auto tagged = codec.EncodeDatagram(0x04D2, 0x162E, payload, peer, {});
    ExpectBytes(tagged, Cat({0xFF, 0xA3, 0x04, 0xD2, 0x16, 0x2E, 0x11, 0x22, 0x33, 0x44}, payload),
                "datagram frame with a communication id: digest flag and 4 bytes");
    Expect(codec.Decode(tagged, peer).kind == Codec::Kind::Datagram,
           "a datagram with the installed communication id is accepted");
    Expect(codec.Decode(datagram, peer).kind == Codec::Kind::Invalid,
           "a datagram without the installed communication id is dropped");
    Expect(codec.Decode(signaling, peer).kind == Codec::Kind::Signaling,
           "signaling never carries the id and still decodes");
    ExpectBytes(codec.EncodeSignaling(handshake, peer), signaling,
                "signaling frames stay plain once the id is installed");
    keyring->SetCommunicationId(std::nullopt);

    // Encrypted and signed datagrams: kind 3 + 1 (crypto) + 2 (signature) = 6; the peer's key
    // unprotects them (p2p_crypto.cpp on the module's OpenSSL).
    P2PKey key;
    for (size_t i = 0; i < key.value.size(); ++i) {
        key.value[i] = static_cast<u8>(0xA0 + i);
    }
    keyring->SetPeerKey(peer, key);
    const auto protected_frame =
        codec.EncodeDatagram(7, 8, payload, peer, Protection{.crypto = true, .signature = true});
    Expect(protected_frame.size() > 6 && protected_frame[0] == 0xFF && protected_frame[1] == 0x86,
           "protected datagram frame: kind 6 (datagram, crypto, signature)");
    Expect(std::memcmp(protected_frame.data() + 6, payload.data(), payload.size()) != 0,
           "protected datagram frame does not carry the payload in clear");
    {
        const auto decoded = codec.Decode(protected_frame, peer);
        Expect(decoded.kind == Codec::Kind::Datagram && decoded.src_vport == 7 &&
                   decoded.dst_vport == 8 && decoded.payload == payload,
               "protected datagram frame decodes back to the payload with the peer's key");
    }
    keyring->RemovePeerKey(peer);
    Expect(codec.Decode(protected_frame, peer).kind == Codec::Kind::Invalid,
           "protected datagram frame is dropped without the key");

    // What the earlier module sent (the shadp2p framing): [src vport][dst vport] + payload, with
    // FFFF/FFFF for signaling. Byte 1 reads as flags with kind 15: not a frame to this codec,
    // which is why its bell offers were never answered by mainline or Windows-port players.
    const auto old_frame = Cat({0xFF, 0xFF, 0xFF, 0xFF}, handshake);
    Expect(codec.Decode(old_frame, peer).kind == Codec::Kind::Invalid,
           "the old [src vport][dst vport] signaling frame is rejected (documented mismatch)");

    if (failures) {
        std::printf("FAIL: %d checks failed\n", failures);
        return 1;
    }
    std::printf("PASS: P2P frames match shadPS4 v0.20.0's FramingCodec\n");
    return 0;
}
