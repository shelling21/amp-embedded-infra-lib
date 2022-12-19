#ifndef PACK_BUILDER_CONFIG_PARSER_HPP
#define PACK_BUILDER_CONFIG_PARSER_HPP

#include "infra/syntax/Json.hpp"

namespace application
{
    class UpgradePackConfigParser
    {
    public:
        class ParseException
            : public std::runtime_error
        {
        public:
            ParseException(const std::string& message)
                : std::runtime_error(message)
            {}
        };
    public:
        UpgradePackConfigParser(infra::JsonObject& json);

        std::vector<std::tuple<std::string, std::string, infra::Optional<uint32_t>>> GetComponents();
        std::vector<std::pair<std::string, std::string>> GetOptions();
        infra::JsonObject GetUpgradeConfiguration();
        std::string GetOutputFilename();
        std::string GetUpgradeKeys();
        std::string GetProductName();
        std::string GetProductVersion();
        std::string GetComponentName();
        uint32_t GetComponentVersion();

    private:
        void CheckMandatoryKeys();
        std::pair<std::string, infra::Optional<uint32_t>> ExtractDataFromObjectComponent(const std::string& key, infra::JsonObject& value);
     
    private:
        infra::JsonObject& json;
    };
}

#endif  // !PACK_BUILDER_CONFIG_PARSER_HPP
