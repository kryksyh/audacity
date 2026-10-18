/*
* Audacity: A Digital Audio Editor
*/
#include "TrackFreeze.h"
#include "WaveTrack.h"
#include "Sequence.h"
#include <map>
#include <stdexcept>
#include "au3-xml/XMLFileReader.h"
#include "au3-xml/XMLWriter.h"

namespace {
const ChannelGroup::Attachments::RegisteredFactory freezeFactory {
    [](auto&) { return std::make_unique<TrackFreeze>(); }
};
const WaveTrackIORegistry::ObjectReaderEntry freezeReader {
    "freeze", [](WaveTrack& track) -> XMLTagHandler* { return &TrackFreeze::Get(track); }
};
const WaveTrackIORegistry::ObjectWriterEntry freezeWriter {
    [](const WaveTrack& track, XMLWriter& writer) { TrackFreeze::Get(track).WriteXML(writer); }
};
}

TrackFreeze& TrackFreeze::Get(WaveTrack& track)
{
    auto& result = track.Attachments::Get<TrackFreeze>(freezeFactory);
    if (!result.mFactory) {
        result.mFactory = track.GetSampleBlockFactory();
    }
    return result;
}

const TrackFreeze& TrackFreeze::Get(const WaveTrack& track)
{
    return Get(const_cast<WaveTrack&>(track));
}

std::unique_ptr<ClientData::Cloneable<> > TrackFreeze::Clone() const
{
    return std::make_unique<TrackFreeze>(*this);
}

bool TrackFreeze::HandleXMLTag(const std::string_view& tag, const AttributesList& attrs)
{
    if (tag == "freeze") {
        sourceXml.clear();
        blocks.clear();
        for (const auto& [name, value] : attrs) {
            if (name == "source") {
                sourceXml = value.ToWString();
            }
        }
        return !sourceXml.empty();
    }
    if (tag != "block") {
        return false;
    }
    long long id = 0;
    long format = 0;
    for (const auto& [name, value] : attrs) {
        if (name == "id" && !value.TryGet(id)) {
            return false;
        } else if (name == "format" && !value.TryGet(format)) {
            return false;
        }
    }
    if (id == 0 || !Sequence::IsValidSampleFormat(format)) {
        return false;
    }
    const auto block = mFactory->CreateFromId(static_cast<sampleFormat>(format), id);
    if (!block) {
        return false;
    }
    blocks.push_back(block);
    return true;
}

XMLTagHandler* TrackFreeze::HandleXMLChild(const std::string_view& tag)
{
    return tag == "block" ? this : nullptr;
}

void TrackFreeze::WriteXML(XMLWriter& writer) const
{
    if (sourceXml.empty()) {
        return;
    }
    writer.StartTag("freeze");
    writer.WriteAttr("source", sourceXml);
    for (const auto& block : blocks) {
        writer.StartTag("block");
        writer.WriteAttr("id", static_cast<long long>(block->GetBlockID()));
        writer.WriteAttr("format", static_cast<long>(block->GetSampleFormat()));
        writer.EndTag("block");
    }
    writer.EndTag("freeze");
}

void TrackFreeze::CopyToFactory(const SampleBlockFactoryPtr& factory)
{
    if (mFactory == factory) {
        return;
    }
    std::vector<SampleBlockPtr> copies;
    std::map<SampleBlockID, SampleBlockID> ids;
    for (const auto& block : blocks) {
        const auto format = block->GetSampleFormat();
        const auto count = block->GetSampleCount();
        SampleBuffer buffer(count, format);
        block->GetSamples(buffer.ptr(), format, 0, count);
        auto copy = factory->Create(buffer.ptr(), count, format);
        ids.emplace(block->GetBlockID(), copy->GetBlockID());
        copies.push_back(std::move(copy));
    }
    class Rewriter final : public XMLTagHandler
    {
    public:
        explicit Rewriter(const std::map<SampleBlockID, SampleBlockID>& ids)
            : ids(ids) {}
        bool HandleXMLTag(const std::string_view& tag, const AttributesList& attrs) override
        {
            writer.StartTag(wxString::FromUTF8(tag.data(), tag.size()));
            for (const auto& [name, value] : attrs) {
                if (name == "blockid") {
                    long long id = 0;
                    if (!value.TryGet(id) || !ids.contains(id)) {
                        return false;
                    }
                    writer.WriteAttr(wxString::FromUTF8(name.data(), name.size()), ids.at(id));
                } else {
                    writer.WriteAttr(wxString::FromUTF8(name.data(), name.size()), value.ToWString());
                }
            }
            return true;
        }

        XMLTagHandler* HandleXMLChild(const std::string_view&) override { return this; }
        void HandleXMLEndTag(const std::string_view& tag) override { writer.EndTag(wxString::FromUTF8(tag.data(), tag.size())); }
        void HandleXMLContent(const std::string_view& text) override
        {
            writer.WriteSubTree(XMLWriter::XMLEsc(wxString::FromUTF8(text.data(), text.size())));
        }
        XMLStringWriter writer;
        const std::map<SampleBlockID, SampleBlockID>& ids;
    } rewriter(ids);
    if (!sourceXml.empty()) {
        XMLFileReader parser;
        if (!parser.ParseString(&rewriter, sourceXml)) {
            throw std::runtime_error("Cannot copy the frozen source to this project.");
        }
        sourceXml = rewriter.writer;
    }
    blocks = std::move(copies);
    mFactory = factory;
}
