/**********************************************************************

  Audacity: A Digital Audio Editor

  @file LV2InstanceFeaturesList.cpp

  Paul Licameli split from LV2Effect.cpp

  Audacity(R) is copyright (c) 1999-2008 Audacity Team.
  License: GPL v2 or later.  See License.txt.

**********************************************************************/

#include "LV2InstanceFeaturesList.h"
#include "LV2Ports.h"
#include "au3-utility/MemoryX.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <wx/filename.h>
#include <wx/log.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/state/state.h>

// Discovery validates instruments through the original LV2 wrapper too. Keep
// their makePath storage alive for that wrapper's entire lifetime, so required
// state features accepted by native instrument playback are also discoverable.
struct LV2InstanceFeaturesList::StatePaths {
   StatePaths()
   {
      const auto lease = wxFileName::CreateTempFileName(wxT("audacity-lv2-"));
      if (lease.empty()) throw std::runtime_error("Could not create LV2 instrument storage.");
      const auto cleanup = finally([&] { wxRemoveFile(lease); });
      const auto directory = lease + wxT(".d");
      root = std::filesystem::u8path(directory.utf8_string());
      if (!wxFileName::Mkdir(directory, 0700)) throw std::runtime_error("Could not create LV2 instrument storage.");
   }
   ~StatePaths()
   {
      std::error_code error;
      std::filesystem::remove_all(root, error);
   }
   static char* makePath(LV2_State_Make_Path_Handle handle, const char* relative) noexcept
   {
      if (!relative) return nullptr;
      try {
         const auto path = std::filesystem::u8path(relative).lexically_normal();
         if (path.has_root_path() || (!path.empty() && *path.begin() == "..")) return nullptr;
         const auto target = static_cast<StatePaths*>(handle)->root / path;
         std::error_code error;
         std::filesystem::create_directories(target.parent_path(), error);
         if (error) return nullptr;
         const auto bytes = target.u8string();
         auto* copy = static_cast<char*>(std::malloc(bytes.size() + 1));
         if (copy) std::memcpy(copy, bytes.c_str(), bytes.size() + 1);
         return copy;
      }
      catch (...) { return nullptr; }
   }
   static void freePath(LV2_State_Free_Path_Handle, char* path) noexcept { std::free(path); }

   std::filesystem::path root;
   LV2_State_Make_Path make { this, makePath };
   LV2_State_Free_Path free { this, freePath };
};

LV2InstanceFeaturesList::LV2InstanceFeaturesList(
   const LV2FeaturesList &baseFeatures
)  : ExtendedLV2FeaturesList{ WithBase, baseFeatures }
   , mOk{ InitializeOptions() }
{
   AddFeature(LV2_OPTIONS__options, mOptions.data());
   InitializeStatePaths();
}

LV2InstanceFeaturesList::~LV2InstanceFeaturesList() = default;

void LV2InstanceFeaturesList::InitializeStatePaths()
{
   const auto features = mBaseFeatures.GetFeaturePointers();
   if (std::any_of(features.begin(), features.end(), [](const auto* feature) {
      return feature && !std::strcmp(feature->URI, LV2_STATE__makePath);
   })) return; // Native playback already supplies its archived resource owner.

   const LilvNodePtr make { lilv_new_uri(LV2Symbols::gWorld, LV2_STATE__makePath) };
   const LilvNodePtr free { lilv_new_uri(LV2Symbols::gWorld, LV2_STATE__freePath) };
   if (!lilv_plugin_has_feature(&mPlug, make.get()) && !lilv_plugin_has_feature(&mPlug, free.get())) return;
   const LV2Ports ports { mPlug };
   if (!ports.mMidiIn || !ports.mAudioOut) return;

   mStatePaths = std::make_unique<StatePaths>();
   AddFeature(LV2_STATE__makePath, &mStatePaths->make);
   AddFeature(LV2_STATE__freePath, &mStatePaths->free);
}

bool LV2InstanceFeaturesList::InitializeOptions()
{
   using namespace LV2Symbols;

   const LilvNodePtr fixed{ lilv_new_uri(gWorld, LV2_BUF_SIZE__fixedBlockLength) };
   const LilvNodePtr power{ lilv_new_uri(gWorld, LV2_BUF_SIZE__powerOf2BlockLength) };
   mPowerOf2BlockLength = lilv_plugin_has_feature(&mPlug, power.get());
   mFixedBlockLength = mPowerOf2BlockLength || lilv_plugin_has_feature(&mPlug, fixed.get());
   if (mFixedBlockLength) AddFeature(LV2_BUF_SIZE__fixedBlockLength, nullptr);
   if (mPowerOf2BlockLength) AddFeature(LV2_BUF_SIZE__powerOf2BlockLength, nullptr);

   // Construct the null-terminated array describing options, and validate it
   AddOption(urid_SequenceSize, sizeof(mSeqSize), urid_Int, &mSeqSize);
   AddOption(urid_MinBlockLength,
      sizeof(mMinimumOption), urid_Int, &mMinimumOption);
   AddOption(urid_MaxBlockLength,
      sizeof(mMaximumOption), urid_Int, &mMaximumOption);
   // Two options are reset later
   mBlockSizeOption = AddOption(urid_NominalBlockLength,
      sizeof(mNominalOption), urid_Int, &mNominalOption);
   AddOption(urid_SampleRate,
      sizeof(mSampleRate), urid_Float, &mSampleRate);
   AddOption(0, 0, 0, nullptr);
   if (!ValidateOptions(lilv_plugin_get_uri(&mPlug)))
      return false;

   // Adjust the values in the block size features according to the plugin
   if (LilvNodePtr minLength{ lilv_world_get(gWorld,
         lilv_plugin_get_uri(&mPlug), node_MinBlockLength, nullptr) }
      ; lilv_node_is_int(minLength.get())
   ){
      if (auto value = lilv_node_as_int(minLength.get())
         ; value >= 0
      )
         mMinBlockSize = std::max<size_t>(mMinBlockSize, value);
   }
   if (LilvNodePtr maxLength{ lilv_world_get(gWorld,
         lilv_plugin_get_uri(&mPlug), node_MaxBlockLength, nullptr) }
      ; lilv_node_is_int(maxLength.get())
   ){
      if (auto value = lilv_node_as_int(maxLength.get())
         ; value >= 1
      )
         mMaxBlockSize = std::min<size_t>(mMaxBlockSize, value);
   }
   mMaxBlockSize = std::max(mMaxBlockSize, mMinBlockSize);
   UpdateBlockSizeOptions();

   return true;
}

void LV2InstanceFeaturesList::UpdateBlockSizeOptions()
{
   mMinimumOption = static_cast<int32_t>(mFixedBlockLength ? mBlockSize : mMinBlockSize);
   mMaximumOption = static_cast<int32_t>(mFixedBlockLength ? mBlockSize : mMaxBlockSize);
   mNominalOption = static_cast<int32_t>(mBlockSize);
}

size_t LV2InstanceFeaturesList::AddOption(
   LV2_URID key, uint32_t size, LV2_URID type, const void *value)
{
   int ndx = mOptions.size();
   if (key != 0)
      mOptions.emplace_back(LV2_Options_Option{
         LV2_OPTIONS_INSTANCE, 0, key, size, type, value });
   else
      mOptions.emplace_back(LV2_Options_Option{});
   return ndx;
}

const LV2_Options_Option *
LV2InstanceFeaturesList::NominalBlockLengthOption() const
{
   if (mSupportsNominalBlockLength)
      return &mOptions[mBlockSizeOption];
   else
      return nullptr;
}

bool LV2InstanceFeaturesList::ValidateOptions(const LilvNode *subject)
{
   return CheckOptions(subject, true) && CheckOptions(subject, false);
}

bool LV2InstanceFeaturesList::CheckOptions(
   const LilvNode *subject, bool required)
{
   using namespace LV2Symbols;
   bool supported = true;
   const auto predicate =
      required ? node_RequiredOption : node_SupportedOption;
   if (LilvNodesPtr nodes{
      lilv_world_find_nodes(gWorld, subject, predicate, nullptr) }
   ){
      LILV_FOREACH(nodes, i, nodes.get()) {
         const auto node = lilv_nodes_get(nodes.get(), i);
         const auto uri = lilv_node_as_string(node);
         // The signature of our constructor justifies this static_cast
         const auto urid = static_cast<const LV2FeaturesList&>(mBaseFeatures)
            .URID_Map(uri);
         if (urid == urid_NominalBlockLength)
            mSupportsNominalBlockLength = true;
         // else if (urid == urid_SampleRate)
            // mSupportsSampleRate = true; // supports changing sample rate
         else if (required) {
            const auto end = mOptions.end();
            supported = (end != std::find_if(mOptions.begin(), end,
               [&](const auto &option){ return option.key == urid; }));
            if (!supported) {
               wxLogError(wxT("%s requires unsupported option %s"),
                  lilv_node_as_string(lilv_plugin_get_uri(&mPlug)), uri);
               break;
            }
         }
      }
   }
   return supported;
}

LV2WrapperFeaturesList::LV2WrapperFeaturesList(
   LV2InstanceFeaturesList &baseFeatures, float sampleRate,
   const LV2_Worker_Schedule *pWorkerSchedule
) : ExtendedLV2FeaturesList{ WithBase, baseFeatures }
{
   baseFeatures.mSampleRate = sampleRate;
   baseFeatures.UpdateBlockSizeOptions();
   auto &base = baseFeatures.Base();
   if (base.SuppliesWorkerInterface()) {
      // Inform the plugin how to send work to another thread
      AddFeature( LV2_WORKER__schedule, pWorkerSchedule );
   }
}
