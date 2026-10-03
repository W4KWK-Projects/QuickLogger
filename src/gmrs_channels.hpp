#pragma once

#include <string>
#include <vector>

namespace ql
{

    // The GMRS channels a GMRS net is set up on (no other frequency can be
    // typed in): simplex channels 1 to 22, then the repeater pairs 15R to
    // 22R, whose repeater listens on the input, 5 MHz above.
    struct GmrsChannel
    {
        const char* name;       // "1" ... "22", "15R" ... "22R"
        const char* frequency;  // MHz, as stored in Net::default_frequency.
        // A repeater pair's input (and its offset, as Net::repeater_offset
        // stores it); blank for a simplex channel.
        const char* input;
        const char* offset;
    };

    // Every channel, in the order above (30 in all).
    const std::vector<GmrsChannel>& GmrsChannels();

    // The index into GmrsChannels() of the channel with `frequency` and
    // `offset` (as a GMRS net stores them), or -1 if none.
    int FindGmrsChannel(const std::string& frequency, const std::string& offset);

    // A channel as shown on screen: "15R  462.5500, input 467.5500" or
    // "3  462.6125".
    std::string DescribeGmrsChannel(const GmrsChannel& channel);

}  // namespace ql
