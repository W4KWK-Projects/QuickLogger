#include "gmrs_channels.hpp"

namespace ql
{

    const std::vector<GmrsChannel>& GmrsChannels()
    {
        // 47 CFR 95.1763.
        static const std::vector<GmrsChannel> channels = {
            {"1", "462.5625", "", ""},
            {"2", "462.5875", "", ""},
            {"3", "462.6125", "", ""},
            {"4", "462.6375", "", ""},
            {"5", "462.6625", "", ""},
            {"6", "462.6875", "", ""},
            {"7", "462.7125", "", ""},
            {"8", "467.5625", "", ""},
            {"9", "467.5875", "", ""},
            {"10", "467.6125", "", ""},
            {"11", "467.6375", "", ""},
            {"12", "467.6625", "", ""},
            {"13", "467.6875", "", ""},
            {"14", "467.7125", "", ""},
            {"15", "462.5500", "", ""},
            {"16", "462.5750", "", ""},
            {"17", "462.6000", "", ""},
            {"18", "462.6250", "", ""},
            {"19", "462.6500", "", ""},
            {"20", "462.6750", "", ""},
            {"21", "462.7000", "", ""},
            {"22", "462.7250", "", ""},
            {"15R", "462.5500", "467.5500", "+5"},
            {"16R", "462.5750", "467.5750", "+5"},
            {"17R", "462.6000", "467.6000", "+5"},
            {"18R", "462.6250", "467.6250", "+5"},
            {"19R", "462.6500", "467.6500", "+5"},
            {"20R", "462.6750", "467.6750", "+5"},
            {"21R", "462.7000", "467.7000", "+5"},
            {"22R", "462.7250", "467.7250", "+5"},
        };
        return channels;
    }

    int FindGmrsChannel(const std::string& frequency, const std::string& offset)
    {
        const std::vector<GmrsChannel>& channels = GmrsChannels();
        for (std::size_t i = 0; i < channels.size(); ++i)
        {
            if (frequency == channels[i].frequency && offset == channels[i].offset)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    std::string DescribeGmrsChannel(const GmrsChannel& channel)
    {
        std::string text = std::string(channel.name) + "  " + channel.frequency;
        if (channel.input[0] != '\0')
        {
            text += ", input ";
            text += channel.input;
        }
        return text;
    }

}  // namespace ql
