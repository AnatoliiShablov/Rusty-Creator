#include "rustwizardpagefactory.h"

#include "rustyconstants.h"
#include "rustsettings.h"
#include "rusttr.h"

#include <coreplugin/generatedfile.h>

#include <utils/algorithm.h>
#include <utils/layoutbuilder.h>
#include <utils/mimeutils.h>
#include <utils/qtcassert.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/target.h>

using namespace ProjectExplorer;
using namespace Utils;

namespace Rusty::Internal {

RustWizardPageFactory::RustWizardPageFactory()
{
    setTypeIdsSuffix("RustConfiguration");
}

WizardPage *RustWizardPageFactory::create(JsonWizard *wizard, Id typeId, const QVariant &data)
{
    Q_UNUSED(wizard)

    QTC_ASSERT(canCreate(typeId), return nullptr);

    QList<QPair<QString, QVariant>> pySideAndData;
    for (const QVariant &item : data.toMap().value("items").toList()) {
        const QMap<QString, QVariant> map = item.toMap();
        const QVariant name = map.value("trKey");
        if (name.isValid())
            pySideAndData.emplaceBack(QPair<QString, QVariant>{name.toString(), map.value("value")});
    }
    bool validIndex = false;
    int defaultPySide = data.toMap().value("index").toInt(&validIndex);
    if (!validIndex)
        defaultPySide = -1;
    return new RustWizardPage(pySideAndData, defaultPySide);
}

static bool validItem(const QVariant &item)
{
    QMap<QString, QVariant> map = item.toMap();
    if (!map.value("trKey").canConvert<QString>())
        return false;
    map = map.value("value").toMap();
    return map.value("PySideVersion").canConvert<QString>();
}

bool RustWizardPageFactory::validateData(Id typeId, const QVariant &data, QString *errorMessage)
{
    QTC_ASSERT(canCreate(typeId), return false);
    const QList<QVariant> items = data.toMap().value("items").toList();

    if (items.isEmpty()) {
        if (errorMessage) {
            *errorMessage = Tr::tr("\"data\" of a Python wizard page expects a map with \"items\" "
                                   "containing a list of objects.");
        }
        return false;
    }

    if (!Utils::allOf(items, &validItem)) {
        if (errorMessage) {
            *errorMessage = Tr::tr(
                "An item of Python wizard page data expects a \"trKey\" field containing the UI "
                "visible string for that Python version and a \"value\" field containing an object "
                "with a \"PySideVersion\" field used for import statements in the Python files.");
        }
        return false;
    }
    return true;
}

RustWizardPage::RustWizardPage(const QList<QPair<QString, QVariant>> &pySideAndData,
                                   const int defaultPyside)
{
    using namespace Layouting;
    m_interpreter.setSettingsDialogId(Rusty::Constants::C_PYTHONOPTIONS_PAGE_ID);
    connect(RustSettings::instance(),
            &RustSettings::interpretersChanged,
            this,
            &RustWizardPage::updateInterpreters);

    m_RsSideVersion.setLabelText(Tr::tr("PySide version:"));
    m_RsSideVersion.setDisplayStyle(SelectionAspect::DisplayStyle::ComboBox);
    for (auto [name, data] : pySideAndData)
        m_RsSideVersion.addOption(SelectionAspect::Option(name, {}, data));
    if (defaultPyside >= 0)
        m_RsSideVersion.setDefaultValue(defaultPyside);

    m_createVenv.setLabelText(Tr::tr("Create new virtual environment"));

    m_venvPath.setLabelText(Tr::tr("Path to virtual environment:"));
    m_venvPath.setEnabler(&m_createVenv);
    m_venvPath.setExpectedKind(PathChooser::Directory);

    m_stateLabel = new InfoLabel();
    m_stateLabel->setWordWrap(true);
    m_stateLabel->setFilled(true);
    m_stateLabel->setType(InfoLabel::Error);
    connect(&m_venvPath, &FilePathAspect::validChanged, this, &RustWizardPage::updateStateLabel);
    connect(&m_createVenv, &BaseAspect::changed, this, &RustWizardPage::updateStateLabel);

    Form {
        m_RsSideVersion, st, br,
        m_interpreter, st, br,
        m_createVenv, st, br,
        m_venvPath, br,
        m_stateLabel, br
    }.attachTo(this);
}

void RustWizardPage::initializePage()
{
    auto wiz = qobject_cast<JsonWizard *>(wizard());
    QTC_ASSERT(wiz, return);
    connect(wiz, &JsonWizard::filesPolished,
            this, &RustWizardPage::setupProject,
            Qt::UniqueConnection);

    const FilePath projectDir = FilePath::fromString(wiz->property("ProjectDirectory").toString());
    m_createVenv.setValue(!projectDir.isEmpty());
    if (m_venvPath().isEmpty())
        m_venvPath.setValue(projectDir.isEmpty() ? FilePath{} : projectDir / "venv");

    updateInterpreters();
    updateStateLabel();
}

bool RustWizardPage::validatePage()
{
    if (m_createVenv() && !m_venvPath.pathChooser()->isValid())
        return false;
    auto wiz = qobject_cast<JsonWizard *>(wizard());
    const QMap<QString, QVariant> data = m_RsSideVersion.itemValue().toMap();
    for (auto it = data.begin(), end = data.end(); it != end; ++it)
        wiz->setValue(it.key(), it.value());
    return true;
}

void RustWizardPage::setupProject(const JsonWizard::GeneratorFiles &files)
{
    for (const JsonWizard::GeneratorFile &f : files) {
        if (f.file.attributes() & Core::GeneratedFile::OpenProjectAttribute) {
            Interpreter interpreter = m_interpreter.currentInterpreter();
            Project *project = ProjectManager::openProject(Utils::mimeTypeForFile(f.file.filePath()),
                                                           f.file.filePath().absoluteFilePath());
            if (m_createVenv()) {
                auto openProjectWithInterpreter = [f](const std::optional<Interpreter> &interpreter) {
                    if (!interpreter)
                        return;
                    Project *project = ProjectManager::projectWithProjectFilePath(f.file.filePath());
                    if (!project)
                        return;
                    if (Target *target = project->activeTarget()) {
                        if (RunConfiguration *rc = target->activeRunConfiguration()) {
                            if (auto interpreters = rc->aspect<InterpreterAspect>())
                                interpreters->setCurrentInterpreter(*interpreter);
                        }
                    }
                };
                RustSettings::createVirtualEnvironment(m_venvPath(),
                                                         interpreter,
                                                         openProjectWithInterpreter,
                                                         project ? project->displayName()
                                                                 : QString{});
            }

            if (project) {
                project->addTargetForDefaultKit();
                if (Target *target = project->activeTarget()) {
                    if (RunConfiguration *rc = target->activeRunConfiguration()) {
                        if (auto interpreters = rc->aspect<InterpreterAspect>()) {
                            interpreters->setCurrentInterpreter(interpreter);
                            project->saveSettings();
                        }
                    }
                }
                delete project;
            }
        }
    }
}

void RustWizardPage::updateInterpreters()
{
    m_interpreter.setDefaultInterpreter(RustSettings::defaultInterpreter());
    m_interpreter.updateInterpreters(RustSettings::interpreters());
}

void RustWizardPage::updateStateLabel()
{
    QTC_ASSERT(m_stateLabel, return);
    if (m_createVenv()) {
        if (PathChooser *pathChooser = m_venvPath.pathChooser()) {
            if (!pathChooser->isValid()) {
                m_stateLabel->show();
                m_stateLabel->setText(pathChooser->errorMessage());
                return;
            }
        }
    }
    m_stateLabel->hide();
}

} // namespace Rusty::Internal

