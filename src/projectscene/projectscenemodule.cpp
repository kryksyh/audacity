/*
* Audacity: A Digital Audio Editor
*/
#include "projectscenemodule.h"

#include "framework/ui/iuiactionsregister.h"
#include "framework/interactive/iinteractiveuriregister.h"

#include "internal/projectsceneuiactions.h"
#include "internal/projectsceneactionscontroller.h"
#include "internal/projectsceneconfiguration.h"
#include "internal/projectviewstatecreator.h"
#include "internal/realtimeeffectpaneltrackselection.h"

#include "view/tracksitemsview/wavepainterproxy.h"
#include "view/tracksitemsview/au3/connectingdotspainter.h"
#include "view/tracksitemsview/au3/minmaxrmspainter.h"
#include "view/tracksitemsview/au3/samplespainter.h"

using namespace au::projectscene;
using namespace muse::modularity;
using namespace muse::ui;
using namespace muse::interactive;

std::string ProjectSceneModule::moduleName() const
{
    return "projectscene";
}

void ProjectSceneModule::registerExports()
{
    m_projectSceneActionsController = std::make_shared<ProjectSceneActionsController>(iocContext());
    m_uiActions = std::make_shared<ProjectSceneUiActions>(iocContext(), m_projectSceneActionsController);
    m_configuration = std::make_shared<ProjectSceneConfiguration>(iocContext());
    m_realtimeEffectPanelTrackSelection = std::make_shared<RealtimeEffectPanelTrackSelection>(iocContext());

    ioc()->registerExport<IProjectSceneConfiguration>(moduleName(), m_configuration);
    ioc()->registerExport<IProjectViewStateCreator>(moduleName(), std::make_shared<ProjectViewStateCreator>(iocContext()));
    ioc()->registerExport<IProjectSceneActionsController>(moduleName(), m_projectSceneActionsController);
    ioc()->registerExport<IRealtimeEffectPanelTrackSelection>(moduleName(), m_realtimeEffectPanelTrackSelection);
    ioc()->registerExport<IWavePainter>(moduleName(), std::make_shared<WavePainterProxy>(iocContext()));
    ioc()->registerExport<IConnectingDotsPainter>(moduleName(), std::make_shared<ConnectingDotsPainter>(iocContext()));
    ioc()->registerExport<IMinMaxRMSPainter>(moduleName(), std::make_shared<MinMaxRMSPainter>(iocContext()));
    ioc()->registerExport<ISamplesPainter>(moduleName(), std::make_shared<SamplesPainter>(iocContext()));
}

void ProjectSceneModule::resolveImports()
{
    auto ir = ioc()->resolve<IInteractiveUriRegister>(moduleName());
    if (ir) {
        ir->registerQmlUri(muse::Uri("audacity://projectscene/editpitchandspeed"),
                           "Audacity.ProjectScene", "PitchAndSpeedChangeDialog");
        ir->registerQmlUri(muse::Uri("audacity://projectscene/insertsilence"),
                           "Audacity.ProjectScene", "InsertSilence");
        ir->registerQmlUri(muse::Uri("audacity://projectscene/openlabeleditor"),
                           "Audacity.ProjectScene", "LabelEditorDialog");
        ir->registerQmlUri(muse::Uri("audacity://projectscene/addnewlabeltrack"),
                           "Audacity.ProjectScene", "AddNewLabelTrackDialog");
    }
}

void ProjectSceneModule::onInit(const muse::IApplication::RunMode& mode)
{
    if (mode != muse::IApplication::RunMode::GuiApp) {
        return;
    }

    m_configuration->init();
    m_projectSceneActionsController->init();
    m_realtimeEffectPanelTrackSelection->init();

    m_uiActions->init();
    auto ar = ioc()->resolve<muse::ui::IUiActionsRegister>(moduleName());
    if (ar) {
        ar->reg(m_uiActions);
    }
}
