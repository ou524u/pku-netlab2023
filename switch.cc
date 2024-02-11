#include "switch.h"
// #include "types.h"

#include <stdint.h>
#include <cstring>
#include <iostream>

// types.h
// const uint8_t FRAME_DELI = 0xDE;

int PackFrame(char* unpacked_frame, char* packed_frame, int frame_length) {
    std::cout << "packing frame" << std::endl;
    // Check for invalid inputs
    if (unpacked_frame == NULL || packed_frame == NULL || frame_length <= 0) {
        return -1;
    }

    int packed_index = 0;
    std::memcpy(packed_frame + packed_index, &FRAME_DELI, 1);
    packed_index++;

    // packed_frame[packed_index++] = FRAME_DELI;  // Start with the delimiter

    // Loop through the original frame
    for (int i = 0; i < frame_length; ++i) {
        if (*(reinterpret_cast<uint8_t*>(unpacked_frame + i)) == FRAME_DELI) {
            // If byte equals delimiter, insert another delimiter
            // packed_frame[packed_index++] = FRAME_DELI;
            std::memcpy(packed_frame + packed_index, &FRAME_DELI, 1);
            packed_index++;
        }
        packed_frame[packed_index++] = unpacked_frame[i];
    }

    // Add even parity byte
    uint8_t parity_byte = 0;
    for (int i = 0; i < packed_index; ++i) {
        uint8_t i_byte = *(reinterpret_cast<uint8_t*>(packed_frame + i));
        for (int k = 0; k < 8; k++) {
            if ((i_byte >> k) & 0x01) {
                parity_byte = parity_byte + 1;
            }
        }
    }
    std::cout << "packing parity_byte num is " << int(parity_byte) << std::endl;
    parity_byte = (parity_byte & 0x01);

    std::memcpy(packed_frame + packed_index, &parity_byte, 1);
    packed_index++;
    return packed_index;
}

int UnpackFrame(char* unpacked_frame, char* packed_frame, int frame_length) {
    // Check for invalid inputs
    if (unpacked_frame == NULL || packed_frame == NULL || frame_length <= 0) {
        return -1;
    }
    // std::cout << "before unpacking frame is " << unpacked_frame << std::endl;

    // Check even parity
    uint8_t parity_byte = 0;
    for (int i = 0; i < frame_length; ++i) {
        uint8_t i_byte = *(reinterpret_cast<uint8_t*>(packed_frame + i));
        for (int k = 0; k < 8; k++) {
            if ((i_byte >> k) & 0x01) {
                parity_byte = parity_byte + 1;
            }
        }
    }

    if (parity_byte & 0x01) {
        // Parity check failed
        // std::cout << "Parity check failed, parity_byte " << int(parity_byte) << std::endl;
        return -1;
    }

    int unpacked_index = 0;
    // Loop through the packed frame to extract the original frame
    for (int i = 1; i < frame_length - 1; ++i) {
        if (*(reinterpret_cast<uint8_t*>(packed_frame + i)) == FRAME_DELI) {
            // Skip the delimiter and check the next byte
            ++i;
            if (*(reinterpret_cast<uint8_t*>(packed_frame + i)) != FRAME_DELI) {
                return -1;
            }
        }
        unpacked_frame[unpacked_index++] = packed_frame[i];
    }

    return unpacked_index;
}

#include <algorithm>
#include <unordered_map>
#include "switch.h"
struct ForwardingEntry {
    int outPort;
    int counter;
    ForwardingEntry() {
        outPort = 0;
        counter = 0;
    }
    ForwardingEntry(int p, int c) {
        outPort = p;
        counter = c;
    }
};

class Switch : public SwitchBase {
   public:
    int numPorts;
    std::unordered_map<uint64_t, ForwardingEntry> forwardingTable;

   public:
    Switch()
        : numPorts(0) {}

    void InitSwitch(int numPorts) override {
        this->numPorts = numPorts;
        forwardingTable.clear();
    }

    int ProcessFrame(int inPort, char* framePtr) override {
        // Check if it's a control frame from the controller
        if (*(reinterpret_cast<uint16_t*>(framePtr + 12)) == ETHER_CONTROL_TYPE) {
            // Aging command, decrement counters and remove entries with counter == 0

            for (auto it = forwardingTable.begin(); it != forwardingTable.end();) {
                it->second.counter = std::max(it->second.counter - 1, 0);
                if (it->second.counter == 0) {
                    it = forwardingTable.erase(it);
                } else {
                    ++it;
                }
            }
            return -1;  // Discard control frame
        }

        // Extract source and destination MAC addresses
        uint64_t destMAC = 0;
        uint64_t srcMAC = 0;

        std::memcpy(&destMAC, framePtr, 6);
        std::memcpy(&srcMAC, framePtr + 6, 6);

        // Check if source MAC is in the forwarding table
        auto it = forwardingTable.find(srcMAC);
        if (it == forwardingTable.end()) {
            // Source MAC not found, add to forwarding table
            ForwardingEntry entry(inPort, 10);  // Initialize counter to 10
            forwardingTable[srcMAC] = entry;
        } else {
            // Source MAC found, update counter
            it->second.counter = 10;
        }

        // Check if destination MAC is in the forwarding table
        it = forwardingTable.find(destMAC);
        if (it == forwardingTable.end()) {
            // Destination MAC not found, broadcast
            return 0;
        }

        // Destination MAC found
        int outPort = it->second.outPort;
        if (outPort == inPort) {
            // Discard frame if outPort equals inPort
            return -1;
        }

        return outPort;
    }
};

SwitchBase* CreateSwitchObject() {
    return new Switch();
}
