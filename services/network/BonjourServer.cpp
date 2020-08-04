#include "services/network/BonjourServer.hpp"
#include "infra/stream/OverwriteStream.hpp"
#include "infra/util/EnumCast.hpp"

namespace services
{
    namespace
    {
        // Since we repeat the question in the answer we can use a pointer to the name
        // in the answer record. The name starts after the DnsRecordHeader.
        const std::array<uint8_t, 2> rnameCompression{ 0xC0, sizeof(DnsRecordHeader) };

        const uint16_t mdnsPort = 5353;
        const IPv4Address mdnsMulticastAddress{ 224, 0, 0, 251 };
    }

    BonjourServer::BonjourServer(DatagramFactory& factory, Multicast& multicast, infra::BoundedConstString instance, infra::BoundedConstString serviceName, infra::BoundedConstString type,
        infra::Optional<IPv4Address> ipv4Address, infra::Optional<IPv6Address> ipv6Address, uint16_t port, const DnsHostnameParts& text)
        : datagramExchange(factory.Listen(*this, mdnsPort, IPVersions::ipv4))
        , multicast(multicast)
        , instance(instance)
        , serviceName(serviceName)
        , type(type)
        , ipv4Address(ipv4Address)
        , ipv6Address(ipv6Address)
        , port(port)
        , text(text)
    {
        multicast.JoinMulticastGroup(datagramExchange, mdnsMulticastAddress);
    }

    BonjourServer::~BonjourServer()
    {
        multicast.LeaveMulticastGroup(datagramExchange, mdnsMulticastAddress);
    }

    void BonjourServer::DataReceived(infra::SharedPtr<infra::StreamReaderWithRewinding>&& reader, UdpSocket from)
    {
        if (GetPort(from) != mdnsPort)
            return;

        if (waitingReader != nullptr)
            return;

        waitingReader = std::move(reader);
        QuestionParser question(*this, *waitingReader);
        if (question.HasAnswer())
            question.RequestSendStream(*datagramExchange, from);
        else
            waitingReader = nullptr;
    }

    void BonjourServer::SendStreamAvailable(infra::SharedPtr<infra::StreamWriter>&& writer)
    {
        assert(waitingReader != nullptr);

        QuestionParser question(*this, *waitingReader, *writer);

        waitingReader = nullptr;
    }

    BonjourServer::Answer::Answer(BonjourServer& server, uint16_t queryId, infra::StreamWriter& writer)
        : server(server)
        , queryId(queryId)
        , writer(writer)
    {
        DnsRecordHeader header{ queryId, DnsRecordHeader::flagsResponse, 0, 0, 0, 0 };
        stream << header;
    }

    std::size_t BonjourServer::Answer::Answers() const
    {
        return answersCount;
    }

    void BonjourServer::Answer::AddAAnswer()
    {
        if (server.ipv4Address != infra::none)
        {
            ++answersCount;
            AddA(DnsHostnameInParts(server.instance)("local"));
        }
    }

    void BonjourServer::Answer::AddAaaaAnswer()
    {
        if (server.ipv6Address != infra::none)
        {
            ++answersCount;
            AddAaaa(DnsHostnameInParts(server.instance)("local"));
        }
    }

    void BonjourServer::Answer::AddPtrAnswer()
    {
        ++answersCount;
        AddPtr(DnsHostnameInParts(server.serviceName)(server.type)("local"));
    }

    void BonjourServer::Answer::AddSrvAnswer()
    {
        ++answersCount;
        AddSrv(DnsHostnameInParts(server.instance)(server.serviceName)(server.type)("local"));
    }

    void BonjourServer::Answer::AddTxtAnswer()
    {
        ++answersCount;
        AddTxt(DnsHostnameInParts(server.instance)(server.serviceName)(server.type)("local"));
    }

    void BonjourServer::Answer::AddAAdditional()
    {
        if (server.ipv4Address != infra::none)
        {
            ++additionalRecordsCount;
            AddA(DnsHostnameInParts(server.instance)("local"));
        }
    }

    void BonjourServer::Answer::AddAaaaAdditional()
    {
        if (server.ipv6Address != infra::none)
        {
            ++additionalRecordsCount;
            AddAaaa(DnsHostnameInParts(server.instance)("local"));
        }
    }

    void BonjourServer::Answer::AddSrvAdditional()
    {
        ++additionalRecordsCount;
        AddSrv(DnsHostnameInParts(server.instance)(server.serviceName)(server.type)("local"));
    }

    void BonjourServer::Answer::AddTxtAdditional()
    {
        ++additionalRecordsCount;
        AddTxt(DnsHostnameInParts(server.instance)(server.serviceName)(server.type)("local"));
    }

    void BonjourServer::Answer::Finish()
    {
        infra::OverwriteDataStream overwriteStream(stream, startMarker);

        DnsRecordHeader header{ queryId, DnsRecordHeader::flagsResponse, 0, answersCount, nameServersCount, additionalRecordsCount };
        overwriteStream << header;
    }

    void BonjourServer::Answer::AddA(const DnsHostnameParts& dnsHostname)
    {
        DnsRecordPayload payload{ DnsType::dnsTypeA, DnsClass::dnsClassIn, std::chrono::seconds(60), sizeof(IPv4Address) };

        stream << infra::text << dnsHostname;
        stream << payload;
        stream << *server.ipv4Address;
    }

    void BonjourServer::Answer::AddAaaa(const DnsHostnameParts& dnsHostname)
    {
        DnsRecordPayload payload{ DnsType::dnsTypeAAAA, DnsClass::dnsClassIn, std::chrono::seconds(60), sizeof(IPv6Address) };

        stream << infra::text << dnsHostname;
        stream << payload;
        stream << *server.ipv6Address;
    }

    void BonjourServer::Answer::AddPtr(const DnsHostnameParts& dnsHostname)
    {
        auto instance = services::DnsHostnameInParts(server.instance)(server.serviceName)(server.type)("local");
        DnsRecordPayload payload{ DnsType::dnsTypePtr, DnsClass::dnsClassIn, std::chrono::seconds(60), static_cast<uint16_t>(instance.StreamedSize()) };

        stream << infra::text << dnsHostname;
        stream << payload;
        stream << infra::text << instance;
    }

    void BonjourServer::Answer::AddSrv(const DnsHostnameParts& dnsHostname)
    {
        auto instance = services::DnsHostnameInParts(server.instance)("local");
        DnsRecordPayload payload{ DnsType::dnsTypeSrv, DnsClass::dnsClassIn, std::chrono::seconds(60), static_cast<uint16_t>(instance.StreamedSize() + 6) };

        infra::BigEndian<uint16_t> priority(0);
        infra::BigEndian<uint16_t> weight(0);
        infra::BigEndian<uint16_t> port(server.port);

        stream << infra::text << dnsHostname;
        stream << payload << priority << weight << port;
        stream << infra::text << instance;
    }

    void BonjourServer::Answer::AddTxt(const DnsHostnameParts& dnsHostname)
    {
        DnsRecordPayload payload{ DnsType::dnsTypeTxt, DnsClass::dnsClassIn, std::chrono::seconds(60), static_cast<uint16_t>(server.text.StreamedSize() - 1) };

        stream << infra::text << dnsHostname;
        stream << payload;
        stream << infra::text << DnsPartsWithoutTermination(server.text);
    }

    BonjourServer::QuestionParser::QuestionParser(BonjourServer& server, infra::StreamReaderWithRewinding& reader)
        : server(server)
        , reader(reader)
    {
        CreateAnswer(countingWriter);
        reader.Rewind(0);
    }

    BonjourServer::QuestionParser::QuestionParser(BonjourServer& server, infra::StreamReaderWithRewinding& reader, infra::StreamWriter& writer)
        : server(server)
        , reader(reader)
    {
        CreateAnswer(writer);
        if (answer != infra::none)
            answer->Finish();
        reader.Rewind(0);
    }

    bool BonjourServer::QuestionParser::HasAnswer() const
    {
        return answer != infra::none && answer->Answers() != 0;
    }

    void BonjourServer::QuestionParser::RequestSendStream(DatagramExchange& datagramExchange, UdpSocket from)
    {
        datagramExchange.RequestSendStream(countingWriter.Processed(), MakeUdpSocket(mdnsMulticastAddress, mdnsPort));
    }

    void BonjourServer::QuestionParser::CreateAnswer(infra::StreamWriter& writer)
    {
        stream >> header;

        if (!IsValidQuestion())
            return;

        answer.Emplace(server, header.id, writer);

        auto startOfQuestions = reader.ConstructSaveMarker();

        for (auto i = 0; valid && i != header.questionsCount; ++i)
            ReadQuestion();

        reader.Rewind(startOfQuestions);

        for (auto i = 0; valid && i != header.questionsCount; ++i)
            ReadQuestionForAdditionalRecords();

        if (!valid)
            answer = infra::none;
    }

    bool BonjourServer::QuestionParser::IsValidQuestion() const
    {
        if (stream.Failed())
            return false;

        if ((header.flags & DnsRecordHeader::flagsOpcodeMask) != DnsRecordHeader::flagsOpcodeQuery)
            return false;

        if ((header.flags & DnsRecordHeader::flagsResponse) != DnsRecordHeader::flagsQuery)
            return false;

        if (header.answersCount != 0)
            return false;

        if (header.nameServersCount != 0)
            return false;

        if (header.additionalRecordsCount != 0)
            return false;

        return true;
    }

    void BonjourServer::QuestionParser::ReadQuestion()
    {
        ReadHostname();
        stream >> footer;

        if (stream.Failed() || !IsFooterValid())
            valid = false;
        else if (IsQueryForMe())
        {
            switch (static_cast<DnsType>(static_cast<uint16_t>(footer.type)))
            {
                case  DnsType::dnsTypeA:
                    answer->AddAAnswer();
                    break;
                case  DnsType::dnsTypeAAAA:
                    answer->AddAaaaAnswer();
                    break;
                case DnsType::dnsTypePtr:
                    answer->AddPtrAnswer();
                    break;
                case DnsType::dnsTypeSrv:
                    answer->AddSrvAnswer();
                    break;
                case DnsType::dnsTypeTxt:
                    answer->AddTxtAnswer();
                    break;
                default:
                    std::abort();
            }
        }
    }

    void BonjourServer::QuestionParser::ReadQuestionForAdditionalRecords()
    {
        ReadHostname();
        stream >> footer;

        if (IsQueryForMe())
        {
            switch (static_cast<DnsType>(static_cast<uint16_t>(footer.type)))
            {
                case DnsType::dnsTypePtr:
                    answer->AddTxtAdditional();
                    answer->AddSrvAdditional();
                    answer->AddAAdditional();
                    answer->AddAaaaAdditional();
                    break;
                case DnsType::dnsTypeSrv:
                    answer->AddAAdditional();
                    answer->AddAaaaAdditional();
                    break;
            }
        }
    }

    bool BonjourServer::QuestionParser::IsFooterValid() const
    {
        if (footer.type != infra::enum_cast(DnsType::dnsTypeA)
            && footer.type != infra::enum_cast(DnsType::dnsTypeAAAA)
            && footer.type != infra::enum_cast(DnsType::dnsTypePtr)
            && footer.type != infra::enum_cast(DnsType::dnsTypeSrv)
            && footer.type != infra::enum_cast(DnsType::dnsTypeTxt))
            return false;

        if (footer.class_ != infra::enum_cast(DnsClass::dnsClassIn))
            return false;

        return true;
    }

    bool BonjourServer::QuestionParser::IsQueryForMe() const
    {
        if (footer.type == infra::enum_cast(DnsType::dnsTypeA) && MyShortInstanceName())
            return true;

        if (footer.type == infra::enum_cast(DnsType::dnsTypeAAAA) && MyShortInstanceName())
            return true;

        if (footer.type == infra::enum_cast(DnsType::dnsTypeSrv) && MyFullInstanceName())
            return true;

        if (footer.type == infra::enum_cast(DnsType::dnsTypeTxt) && MyFullInstanceName())
            return true;

        if (footer.type == infra::enum_cast(DnsType::dnsTypePtr) && MyServiceName())
            return true;

        return false;
    }

    bool BonjourServer::QuestionParser::MyFullInstanceName() const
    {
        std::array<infra::BoundedConstString, 4> service = { { server.instance, server.serviceName, server.type, "local" } };

        if (HostNameIs(service))
            return true;

        return false;
    }

    bool BonjourServer::QuestionParser::MyShortInstanceName() const
    {
        std::array<infra::BoundedConstString, 2> service = { { server.instance, "local" } };

        if (HostNameIs(service))
            return true;

        return false;
    }

    bool BonjourServer::QuestionParser::MyServiceName() const
    {
        std::array<infra::BoundedConstString, 3> service = { { server.serviceName, server.type, "local" } };

        if (HostNameIs(service))
            return true;

        return false;
    }

    void BonjourServer::QuestionParser::ReadHostname()
    {
        auto hostnameStart = reader.ConstructSaveMarker();
        DnsHostnamePartsStream hostnameParts(reader, hostnameStart);

        reconstructedHostname.clear();

        while (!hostnameParts.Empty())
        {
            reconstructedHostname += hostnameParts.Current();
            hostnameParts.ConsumeCurrent();

            if (!hostnameParts.Empty())
                reconstructedHostname += '.';
        }

        hostnameParts.ConsumeStream();
    }

    bool BonjourServer::QuestionParser::HostNameIs(infra::MemoryRange<infra::BoundedConstString> components) const
    {
        DnsHostnamePartsString parts(reconstructedHostname);

        while (!components.empty() && !parts.Empty() && parts.Current() == components.front())
        {
            parts.ConsumeCurrent();
            components.pop_front();
        }

        return parts.Empty() && components.empty();
    }
}
