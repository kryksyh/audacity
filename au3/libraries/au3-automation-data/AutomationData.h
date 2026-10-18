/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <string>
#include <vector>
#include "au3-xml/XMLTagHandler.h"

struct AutomationPointData {
    double time = 0.0;
    double value = 1.0;
    double curve = 0.0;
    bool operator==(const AutomationPointData&) const = default;
};

struct AutomationLaneData {
    std::string parameter;
    bool enabled = true;
    std::vector<AutomationPointData> points;
    bool operator==(const AutomationLaneData&) const = default;
};

class AUTOMATION_DATA_API AutomationData final : public XMLTagHandler
{
public:
    static constexpr auto XMLTag = "automation";
    const std::vector<AutomationLaneData>& Lanes() const { return mLanes; }
    const AutomationLaneData* Find(const std::string& parameter) const;
    double ValueAt(const std::string& parameter, double time, double defaultValue) const;
    static double ValueAt(const AutomationLaneData* lane, double time, double defaultValue);
    bool SetLane(AutomationLaneData lane);
    bool RemoveLane(const std::string& parameter);
    void Clear() { mLanes.clear(); }
    bool HandleXMLTag(const std::string_view& tag, const AttributesList& attrs) override;
    XMLTagHandler* HandleXMLChild(const std::string_view& tag) override;
    void WriteXML(XMLWriter& writer) const;

private:
    std::vector<AutomationLaneData> mLanes;
};
