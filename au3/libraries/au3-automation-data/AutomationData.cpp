/*
* Audacity: A Digital Audio Editor
*/
#include "AutomationData.h"

#include <algorithm>
#include <cmath>
#include "au3-xml/XMLWriter.h"

namespace {
bool validPoint(const AutomationPointData& point)
{
    return std::isfinite(point.time) && point.time >= 0.0 && std::isfinite(point.value) && point.value >= 0.0
           && point.value <= 1.0 && std::isfinite(point.curve) && std::abs(point.curve) <= 1.0;
}
}

const AutomationLaneData* AutomationData::Find(const std::string& parameter) const
{
    const auto lane = std::find_if(mLanes.begin(), mLanes.end(), [&](const auto& lane) { return lane.parameter == parameter; });
    return lane == mLanes.end() ? nullptr : &*lane;
}

bool AutomationData::SetLane(AutomationLaneData lane)
{
    if (lane.parameter.empty() || !std::all_of(lane.points.begin(), lane.points.end(), validPoint)) {
        return false;
    }
    std::sort(lane.points.begin(), lane.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    for (size_t i = 1; i < lane.points.size(); ++i) {
        if (lane.points[i - 1].time == lane.points[i].time) {
            return false;
        }
    }
    const auto existing = std::find_if(mLanes.begin(), mLanes.end(), [&](const auto& entry) { return entry.parameter == lane.parameter; });
    if (existing == mLanes.end()) {
        mLanes.push_back(std::move(lane));
    } else {
        *existing = std::move(lane);
    }
    return true;
}

bool AutomationData::RemoveLane(const std::string& parameter)
{
    return std::erase_if(mLanes, [&](const auto& lane) { return lane.parameter == parameter; }) != 0;
}

bool AutomationData::HandleXMLTag(const std::string_view& tag, const AttributesList& attrs)
{
    if (tag == XMLTag) {
        Clear();
        return true;
    }
    if (tag == "lane") {
        AutomationLaneData lane;
        for (const auto& [name, value] : attrs) {
            if (name == "parameter") {
                lane.parameter = value.ToWString().utf8_string();
            } else if (name == "enabled" && !value.TryGet(lane.enabled)) {
                return false;
            }
        }
        if (Find(lane.parameter)) {
            return false;
        }
        return SetLane(std::move(lane));
    }
    if (tag != "point" || mLanes.empty()) {
        return false;
    }
    AutomationPointData point;
    bool hasTime = false, hasValue = false;
    for (const auto& [name, value] : attrs) {
        if (name == "time") {
            hasTime = value.TryGet(point.time);
        } else if (name == "value") {
            hasValue = value.TryGet(point.value);
        } else if (name == "curve" && !value.TryGet(point.curve)) {
            return false;
        }
    }
    auto& points = mLanes.back().points;
    if (!hasTime || !hasValue || !validPoint(point) || (!points.empty() && point.time <= points.back().time)) {
        return false;
    }
    points.push_back(point);
    return true;
}

XMLTagHandler* AutomationData::HandleXMLChild(const std::string_view& tag)
{
    return tag == "lane" || tag == "point" ? this : nullptr;
}

void AutomationData::WriteXML(XMLWriter& writer) const
{
    if (mLanes.empty()) {
        return;
    }
    writer.StartTag(XMLTag);
    for (const auto& lane : mLanes) {
        writer.StartTag("lane");
        writer.WriteAttr("parameter", wxString::FromUTF8(lane.parameter));
        writer.WriteAttr("enabled", lane.enabled);
        for (const auto& point : lane.points) {
            writer.StartTag("point");
            writer.WriteAttr("time", point.time, 17);
            writer.WriteAttr("value", point.value, 17);
            writer.WriteAttr("curve", point.curve, 17);
            writer.EndTag("point");
        }
        writer.EndTag("lane");
    }
    writer.EndTag(XMLTag);
}
