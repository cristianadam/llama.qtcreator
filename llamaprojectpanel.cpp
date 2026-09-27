#include "llamaprojectpanel.h"

#include "llamaconstants.h"
#include "llamasettings.h"
#include "llamatr.h"

#include <projectexplorer/project.h>
#include <projectexplorer/projectpanelfactory.h>

#include <utils/layoutbuilder.h>

#include <QWidget>

using namespace ProjectExplorer;

namespace LlamaCpp {

class LlamaCppProjectSettingsWidget final : public QWidget
{
public:
    explicit LlamaCppProjectSettingsWidget(Project *project)
    {
        using namespace Layouting;

        m_settings = new LlamaProjectSettings(project);
        m_settings->setParent(this);

        Column {
            m_settings->useGlobalSettings,
            m_settings->enableLlamaCpp,
            m_settings->sandboxCommands,
            st,
        }.attachTo(this);

        applyGlobalState();
        m_settings->useGlobalSettings.addOnChanged(this, [this] { applyGlobalState(); });
    }

private:
    void applyGlobalState()
    {
        const bool useGlobal = m_settings->useGlobalSettings();
        m_settings->enableLlamaCpp.setEnabled(!useGlobal);
        m_settings->sandboxCommands.setEnabled(!useGlobal);
    }

    LlamaProjectSettings *m_settings = nullptr;
};

static QWidget *createLlamaCppProjectPanel(Project *project)
{
    return new LlamaCppProjectSettingsWidget(project);
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
