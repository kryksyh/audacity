/*
* Audacity: A Digital Audio Editor
*/
#include "projectscenemodule.h"

#include <QtQml>

#include "types/projectscenetypes.h"

#include "framework/ui/iuiactionsregister.h"
#include "framework/interactive/iinteractiveuriregister.h"

#include "internal/projectsceneuiactions.h"
#include "internal/projectsceneactionscontroller.h"
#include "internal/projectsceneconfiguration.h"
#include "internal/projectviewstatecreator.h"
#include "internal/realtimeeffectpaneltrackselection.h"

#include "qml/Audacity/ProjectScene/view/tracksitemsview/trackclipitem.h"
#include "qml/Audacity/ProjectScene/view/tracksitemsview/tracklabelitem.h"
#include "qml/Audacity/ProjectScene/view/tracksitemsview/wavepainterproxy.h"
#include "qml/Audacity/ProjectScene/view/tracksitemsview/au3/connectingdotspainter.h"
#include "qml/Audacity/ProjectScene/view/tracksitemsview/au3/minmaxrmspainter.h"
#include "qml/Audacity/ProjectScene/view/tracksitemsview/au3/samplespainter.h"
#include "qml/Audacity/ProjectScene/view/toolbars/playbacktoolbarcustomiseitem.h"

using namespace au::projectscene;
using namespace muse::modularity;
using namespace muse::ui;
using namespace muse::interactive;

static void projectscene_init_qrc()
{
    Q_INIT_RESOURCE(projectscene);
}

std::string ProjectSceneModule::moduleName() const
{
    return "projectscene";
}

void ProjectSceneModule::registerResources()
{
    projectscene_init_qrc();
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
                           "Audacity/ProjectScene/tracksitemsview/pitchandspeed/PitchAndSpeedChangeDialog.qml");
        ir->registerQmlUri(muse::Uri("audacity://projectscene/insertsilence"),
                           "Audacity/ProjectScene/common/InsertSilence.qml");
        ir->registerQmlUri(muse::Uri("audacity://projectscene/openlabeleditor"),
                           "Audacity/ProjectScene/tracksitemsview/labeleditor/LabelEditorDialog.qml");
        ir->registerQmlUri(muse::Uri("audacity://projectscene/addnewlabeltrack"),
                           "Audacity/ProjectScene/tracksitemsview/labeleditor/AddNewLabelTrackDialog.qml");
    }
}

void ProjectSceneModule::registerUiTypes()
{
    // types
    qmlRegisterUncreatableType<TrackTypes>("Audacity.ProjectScene", 1, 0, "TrackType", "Not creatable from QML");
    qmlRegisterUncreatableType<ClipKey>("Audacity.ProjectScene", 1, 0, "ClipKey", "Not creatable from QML");
    qmlRegisterUncreatableType<ClipStyles>("Audacity.ProjectScene", 1, 0, "ClipStyle", "Not creatable from QML");
    qmlRegisterUncreatableType<StereoHeightsPref>("Audacity.ProjectScene", 1, 0, "AsymmetricStereoHeights", "Not creatable from QML");
    qmlRegisterUncreatableType<ClipBoundary>("Audacity.ProjectScene", 1, 0, "ClipBoundaryAction", "Not creatable from QML");
    qmlRegisterUncreatableType<DirectionType>("Audacity.ProjectScene", 1, 0, "Direction", "Not creatable from QML");
    qmlRegisterUncreatableType<PlaybackToolBarCustomiseItem>("Audacity.ProjectScene", 1, 0, "PlaybackToolBarCustomiseItem",
                                                             "Cannot create");

    qmlRegisterUncreatableType<TrackClipItem>("Audacity.ProjectScene", 1, 0, "TrackClipItem", "Not creatable from QML");
    qmlRegisterUncreatableType<TrackLabelItem>("Audacity.ProjectScene", 1, 0, "TrackLabelItem", "Not creatable from QML");
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
