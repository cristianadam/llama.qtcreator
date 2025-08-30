#include "llamaprojectpanel.h"

#include "llamaconstants.h"
#include "llamasettings.h"
#include "llamatr.h"

#include <projectexplorer/project.h>
#include <projectexplorer/projectpanelfactory.h>
#include <projectexplorer/projectsettingswidget.h>

#include <utils/layoutbuilder.h>

using namespace ProjectExplorer;

namespace LlamaCpp {

class LlamaCppProjectSettingsWidget final : public ProjectSettingsWidget
{
public:
    LlamaCppProjectSettingsWidget()
    {
        setGlobalSettingsId(Constants::LLAMACPP_GENERAL_OPTIONS_ID);
        setUseGlobalSettingsCheckBoxVisible(true);
    }
};

static ProjectSettingsWidget *createLlamaCppProjectPanel(Project *project)
{
    using namespace Layouting;

    auto widget = new LlamaCppProjectSettingsWidget;
    auto settings = new LlamaProjectSettings(project);
    settings->setParent(widget);

    QObject::connect(widget,
                     &ProjectSettingsWidget::useGlobalSettingsChanged,
                     settings,
                     &LlamaProjectSettings::setUseGlobalSettings);

    widget->setUseGlobalSettings(settings->useGlobalSettings());
    widget->setEnabled(!settings->useGlobalSettings());

    QObject::connect(widget,
                     &ProjectSettingsWidget::useGlobalSettingsChanged,
                     widget,
                     [widget](bool useGlobal) { widget->setEnabled(!useGlobal); });

    // clang-format off
        Column {
            settings->enableLlamaCpp,
        }.attachTo(widget);
    // clang-format on

    return widget;
}

class LlamaCppProjectPanelFactory final : public ProjectPanelFactory
{
public:
    LlamaCppProjectPanelFactory()
    {
        setPriority(1000);
        setDisplayName(Tr::tr("llama.cpp"));
        setCreateWidgetFunction(&createLlamaCppProjectPanel);
    }
};

void setupLlamaCppProjectPanel()
{
    static LlamaCppProjectPanelFactory theLlamaCppProjectPanelFactory;
}

} // namespace LlamaCpp
