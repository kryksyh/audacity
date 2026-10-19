/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include <vector>
#include <wx/string.h>
#include "SampleBlock.h"
#include "au3-registries/ClientData.h"
#include "au3-xml/XMLTagHandler.h"

class WaveTrack;
class XMLWriter;

class WAVE_TRACK_API TrackFreeze final : public ClientData::Cloneable<>, public XMLTagHandler
{
public:
    static TrackFreeze& Get(WaveTrack& track);
    static const TrackFreeze& Get(const WaveTrack& track);
    std::unique_ptr<ClientData::Cloneable<>> Clone() const override;
    bool HandleXMLTag(const std::string_view& tag, const AttributesList& attrs) override;
    XMLTagHandler* HandleXMLChild(const std::string_view& tag) override;
    void WriteXML(XMLWriter& writer) const;
    void CopyToFactory(const SampleBlockFactoryPtr& factory);

    wxString sourceXml;
    std::vector<SampleBlockPtr> blocks;

private:
    SampleBlockFactoryPtr mFactory;
};
