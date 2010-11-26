/****************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of Qt Creator.
**
** $QT_BEGIN_LICENSE:LGPL$
** No Commercial Usage
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "maemopackagecreationstep.h"

#include "maemoconstants.h"
#include "maemodeployables.h"
#include "maemodeploystep.h"
#include "maemoglobal.h"
#include "maemopackagecreationwidget.h"
#include "maemotemplatesmanager.h"
#include "maemotoolchain.h"

#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <qt4buildconfiguration.h>
#include <qt4project.h>
#include <qt4target.h>
#include <utils/environment.h>

#include <QtCore/QDateTime>
#include <QtCore/QProcess>
#include <QtCore/QRegExp>
#include <QtCore/QStringBuilder>
#include <QtGui/QWidget>

namespace {
    const QLatin1String PackagingEnabledKey("Packaging Enabled");
    const QLatin1String MagicFileName(".qtcreator");
}

using namespace ProjectExplorer::Constants;
using ProjectExplorer::BuildStepList;
using ProjectExplorer::BuildStepConfigWidget;
using ProjectExplorer::Task;

namespace Qt4ProjectManager {
namespace Internal {

const QLatin1String MaemoPackageCreationStep::DefaultVersionNumber("0.0.1");

MaemoPackageCreationStep::MaemoPackageCreationStep(BuildStepList *bsl)
    : ProjectExplorer::BuildStep(bsl, CreatePackageId),
      m_packagingEnabled(true)
{
    ctor();
}

MaemoPackageCreationStep::MaemoPackageCreationStep(BuildStepList *bsl,
    MaemoPackageCreationStep *other)
    : BuildStep(bsl, other),
      m_packagingEnabled(other->m_packagingEnabled)
{
    ctor();
}

MaemoPackageCreationStep::~MaemoPackageCreationStep()
{
}

void MaemoPackageCreationStep::ctor()
{
    setDefaultDisplayName(tr("Packaging for Maemo"));

    m_lastBuildConfig = qt4BuildConfiguration();
    connect(target(),
        SIGNAL(activeBuildConfigurationChanged(ProjectExplorer::BuildConfiguration*)),
        this, SLOT(handleBuildConfigChanged()));
    handleBuildConfigChanged();
}

bool MaemoPackageCreationStep::init()
{
    return true;
}

QVariantMap MaemoPackageCreationStep::toMap() const
{
    QVariantMap map(ProjectExplorer::BuildStep::toMap());
    map.insert(PackagingEnabledKey, m_packagingEnabled);
    return map;
}

bool MaemoPackageCreationStep::fromMap(const QVariantMap &map)
{
    m_packagingEnabled = map.value(PackagingEnabledKey, true).toBool();
    return ProjectExplorer::BuildStep::fromMap(map);
}

void MaemoPackageCreationStep::run(QFutureInterface<bool> &fi)
{
    bool success;
    if (m_packagingEnabled) {
        // TODO: Make the build process asynchronous; i.e. no waitFor()-functions etc.
        QProcess * const buildProc = new QProcess;
        connect(buildProc, SIGNAL(readyReadStandardOutput()), this,
            SLOT(handleBuildOutput()));
        connect(buildProc, SIGNAL(readyReadStandardError()), this,
            SLOT(handleBuildOutput()));
        success = createPackage(buildProc);
        disconnect(buildProc, 0, this, 0);
        buildProc->deleteLater();
    } else  {
        success = true;
    }
    fi.reportResult(success);
}

BuildStepConfigWidget *MaemoPackageCreationStep::createConfigWidget()
{
    return new MaemoPackageCreationWidget(this);
}

bool MaemoPackageCreationStep::createPackage(QProcess *buildProc)
{
    if (!packagingNeeded()) {
        emit addOutput(tr("Package up to date."), MessageOutput);
        return true;
    }

    emit addOutput(tr("Creating package file ..."), MessageOutput);
    checkProjectName();
    QString error;
    if (!preparePackagingProcess(buildProc, qt4BuildConfiguration(),
       buildDirectory(), &error)) {
        raiseError(error);
        return false;
    }

    const QString projectDir
        = buildConfiguration()->target()->project()->projectDirectory();
    const bool inSourceBuild
        = QFileInfo(buildDirectory()) == QFileInfo(projectDir);
    if (!copyDebianFiles(inSourceBuild))
        return false;

    if (!runCommand(buildProc, QLatin1String("dpkg-buildpackage -nc -uc -us")))
        return false;

    // Workaround for non-working dh_builddeb --destdir=.
    if (!QDir(buildDirectory()).isRoot()) {
        const ProjectExplorer::Project * const project
            = buildConfiguration()->target()->project();
        QString error;
        const QString pkgFileName = packageFileName(project,
            MaemoTemplatesManager::instance()->version(project, &error));
        if (!error.isEmpty())
            raiseError(tr("Packaging failed."), error);
        const QString changesFileName = QFileInfo(pkgFileName)
            .completeBaseName() + QLatin1String(".changes");
        const QString packageSourceDir = buildDirectory() + QLatin1String("/../");
        const QString packageSourceFilePath
            = packageSourceDir + pkgFileName;
        const QString changesSourceFilePath
            = packageSourceDir + changesFileName;
        const QString changesTargetFilePath
            = buildDirectory() + QLatin1Char('/') + changesFileName;
        QFile::remove(packageFilePath());
        QFile::remove(changesTargetFilePath);
        if (!QFile::rename(packageSourceFilePath, packageFilePath())
            || !QFile::rename(changesSourceFilePath, changesTargetFilePath)) {
            raiseError(tr("Packaging failed."),
                tr("Could not move package files from %1 to %2.")
                .arg(packageSourceDir, buildDirectory()));
            return false;
        }
    }

    emit addOutput(tr("Package created."), BuildStep::MessageOutput);
    deployStep()->deployables()->setUnmodified();
    if (inSourceBuild) {
        buildProc->start(packagingCommand(maemoToolChain(),
            QLatin1String("dh_clean")));
        buildProc->waitForFinished();
        buildProc->terminate();
    }
    return true;
}

bool MaemoPackageCreationStep::copyDebianFiles(bool inSourceBuild)
{
    const QString debianDirPath = buildDirectory() + QLatin1String("/debian");
    const QString magicFilePath
        = debianDirPath + QLatin1Char('/') + MagicFileName;
    if (inSourceBuild && QFileInfo(debianDirPath).isDir()
        && !QFileInfo(magicFilePath).exists()) {
        raiseError(tr("Packaging failed: Foreign debian directory detected."),
             tr("You are not using a shadow build and there is a debian "
                "directory in your project root ('%1'). Qt Creator will not "
                "overwrite that directory. Please remove it or use the "
                "shadow build feature.")
                   .arg(QDir::toNativeSeparators(debianDirPath)));
        return false;
    }
    if (!removeDirectory(debianDirPath)) {
        raiseError(tr("Packaging failed."),
            tr("Could not remove directory '%1'.").arg(debianDirPath));
        return false;
    }
    QDir buildDir(buildDirectory());
    if (!buildDir.mkdir("debian")) {
        raiseError(tr("Could not create Debian directory '%1'.")
                   .arg(debianDirPath));
        return false;
    }
    const QString templatesDirPath = MaemoTemplatesManager::instance()
        ->debianDirPath(buildConfiguration()->target()->project());
    QDir templatesDir(templatesDirPath);
    const QStringList &files = templatesDir.entryList(QDir::Files);
    foreach (const QString &fileName, files) {
        const QString srcFile
                = templatesDirPath + QLatin1Char('/') + fileName;
        const QString destFile
                = debianDirPath + QLatin1Char('/') + fileName;
        if (!QFile::copy(srcFile, destFile)) {
            raiseError(tr("Could not copy file '%1' to '%2'")
                       .arg(QDir::toNativeSeparators(srcFile),
                            QDir::toNativeSeparators(destFile)));
            return false;
        }

        if (fileName == QLatin1String("rules"))
            updateDesktopFiles(destFile);
    }

    QFile magicFile(magicFilePath);
    if (!magicFile.open(QIODevice::WriteOnly)) {
        raiseError(tr("Error: Could not create file '%1'.")
            .arg(QDir::toNativeSeparators(magicFilePath)));
        return false;
    }

    return true;
}

bool MaemoPackageCreationStep::removeDirectory(const QString &dirPath)
{
    QDir dir(dirPath);
    if (!dir.exists())
        return true;

    const QStringList &files
        = dir.entryList(QDir::Files | QDir::Hidden | QDir::System);
    foreach (const QString &fileName, files) {
        if (!dir.remove(fileName))
            return false;
    }

    const QStringList &subDirs
        = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    foreach (const QString &subDirName, subDirs) {
        if (!removeDirectory(dirPath + QLatin1Char('/') + subDirName))
            return false;
    }

    return dir.rmdir(dirPath);
}

bool MaemoPackageCreationStep::runCommand(QProcess *buildProc,
    const QString &command)
{
    emit addOutput(tr("Package Creation: Running command '%1'.").arg(command), BuildStep::MessageOutput);
    buildProc->start(packagingCommand(maemoToolChain(), command));
    if (!buildProc->waitForStarted()) {
        raiseError(tr("Packaging failed."),
            tr("Packaging error: Could not start command '%1'. Reason: %2")
            .arg(command).arg(buildProc->errorString()));
        return false;
    }
    buildProc->waitForFinished(-1);
    if (buildProc->error() != QProcess::UnknownError
        || buildProc->exitCode() != 0) {
        QString mainMessage = tr("Packaging Error: Command '%1' failed.")
            .arg(command);
        if (buildProc->error() != QProcess::UnknownError)
            mainMessage += tr(" Reason: %1").arg(buildProc->errorString());
        else
            mainMessage += tr("Exit code: %1").arg(buildProc->exitCode());
        raiseError(mainMessage);
        return false;
    }
    return true;
}

void MaemoPackageCreationStep::handleBuildOutput()
{
    QProcess * const buildProc = qobject_cast<QProcess *>(sender());
    if (!buildProc)
        return;
    const QByteArray &stdOut = buildProc->readAllStandardOutput();
    if (!stdOut.isEmpty())
        emit addOutput(QString::fromLocal8Bit(stdOut), BuildStep::NormalOutput);
    const QByteArray &errorOut = buildProc->readAllStandardError();
    if (!errorOut.isEmpty()) {
        emit addOutput(QString::fromLocal8Bit(errorOut), BuildStep::ErrorOutput);
    }
}

void MaemoPackageCreationStep::handleBuildConfigChanged()
{
    if (m_lastBuildConfig)
        disconnect(m_lastBuildConfig, 0, this, 0);
    m_lastBuildConfig = qt4BuildConfiguration();
    connect(m_lastBuildConfig, SIGNAL(qtVersionChanged()), this,
        SIGNAL(qtVersionChanged()));
    connect(m_lastBuildConfig, SIGNAL(buildDirectoryChanged()), this,
        SIGNAL(packageFilePathChanged()));
    emit qtVersionChanged();
    emit packageFilePathChanged();
}

const Qt4BuildConfiguration *MaemoPackageCreationStep::qt4BuildConfiguration() const
{
    return static_cast<Qt4BuildConfiguration *>(buildConfiguration());
}

QString MaemoPackageCreationStep::buildDirectory() const
{
    return qt4BuildConfiguration()->buildDirectory();
}

QString MaemoPackageCreationStep::projectName() const
{
    return qt4BuildConfiguration()->qt4Target()->qt4Project()
        ->rootProjectNode()->displayName().toLower();
}

const MaemoToolChain *MaemoPackageCreationStep::maemoToolChain() const
{
    return static_cast<MaemoToolChain *>(qt4BuildConfiguration()->toolChain());
}

MaemoDeployStep *MaemoPackageCreationStep::deployStep() const
{
    MaemoDeployStep * const deployStep
        = MaemoGlobal::buildStep<MaemoDeployStep>(target()->activeDeployConfiguration());
    Q_ASSERT(deployStep &&
        "Fatal error: Maemo build configuration without deploy step.");
    return deployStep;
}

QString MaemoPackageCreationStep::maddeRoot() const
{
    return maemoToolChain()->maddeRoot();
}

QString MaemoPackageCreationStep::targetRoot() const
{
    return maemoToolChain()->targetRoot();
}

bool MaemoPackageCreationStep::packagingNeeded() const
{
    const QSharedPointer<MaemoDeployables> &deployables
        = deployStep()->deployables();
    QFileInfo packageInfo(packageFilePath());
    if (!packageInfo.exists() || deployables->isModified())
        return true;

    const int deployableCount = deployables->deployableCount();
    for (int i = 0; i < deployableCount; ++i) {
        if (isFileNewerThan(deployables->deployableAt(i).localFilePath,
                packageInfo.lastModified()))
            return true;
    }

    const ProjectExplorer::Project * const project = target()->project();
    const MaemoTemplatesManager * const templatesManager
        = MaemoTemplatesManager::instance();
    const QString debianPath = templatesManager->debianDirPath(project);
    if (packageInfo.lastModified() <= QFileInfo(debianPath).lastModified())
        return true;
    const QStringList debianFiles = templatesManager->debianFiles(project);
    foreach (const QString &debianFile, debianFiles) {
        const QString absFilePath = debianPath + QLatin1Char('/') + debianFile;
        if (packageInfo.lastModified() <= QFileInfo(absFilePath).lastModified())
            return true;
    }

    return false;
}

bool MaemoPackageCreationStep::isFileNewerThan(const QString &filePath,
    const QDateTime &timeStamp) const
{
    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists() || fileInfo.lastModified() >= timeStamp)
        return true;
    if (fileInfo.isDir()) {
        const QStringList dirContents = QDir(filePath)
            .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        foreach (const QString &curFileName, dirContents) {
            const QString curFilePath
                = filePath + QLatin1Char('/') + curFileName;
            if (isFileNewerThan(curFilePath, timeStamp))
                return true;
        }
    }
    return false;
}

QString MaemoPackageCreationStep::packageFilePath() const
{
    QString error;
    const QString &version = versionString(&error);
    if (version.isEmpty())
        return QString();
    return buildDirectory() % '/'
        % packageFileName(buildConfiguration()->target()->project(), version);
}

bool MaemoPackageCreationStep::isPackagingEnabled() const
{
    return m_packagingEnabled || !maemoToolChain()->allowsPackagingDisabling();
}

QString MaemoPackageCreationStep::versionString(QString *error) const
{
    return MaemoTemplatesManager::instance()
        ->version(buildConfiguration()->target()->project(), error);

}

bool MaemoPackageCreationStep::setVersionString(const QString &version,
    QString *error)
{
    const bool success = MaemoTemplatesManager::instance()
        ->setVersion(buildConfiguration()->target()->project(), version, error);
    if (success)
        emit packageFilePathChanged();
    return success;
}

QString MaemoPackageCreationStep::nativePath(const QFile &file)
{
    return QDir::toNativeSeparators(QFileInfo(file).filePath());
}

void MaemoPackageCreationStep::raiseError(const QString &shortMsg,
                                          const QString &detailedMsg)
{
    emit addOutput(detailedMsg.isNull() ? shortMsg : detailedMsg, BuildStep::ErrorOutput);
    emit addTask(Task(Task::Error, shortMsg, QString(), -1,
                      TASK_CATEGORY_BUILDSYSTEM));
}

bool MaemoPackageCreationStep::preparePackagingProcess(QProcess *proc,
    const Qt4BuildConfiguration *bc, const QString &workingDir, QString *error)
{
    const MaemoToolChain * const tc
        = dynamic_cast<const MaemoToolChain *>(bc->toolChain());
    if (!tc) {
        *error = tr("Build configuration has no Maemo toolchain.");
        return false;
    }
    QFile configFile(tc->targetRoot() % QLatin1String("/config.sh"));
    if (!configFile.open(QIODevice::ReadOnly)) {
        *error = tr("Cannot open MADDE config file '%1'.")
            .arg(nativePath(configFile));
        return false;
    }

    Utils::Environment env = bc->environment();
    const QString &path
        = QDir::toNativeSeparators(tc->maddeRoot() + QLatin1Char('/'));
#ifdef Q_OS_WIN
    env.prependOrSetPath(path % QLatin1String("bin"));
#endif
    env.prependOrSetPath(tc->targetRoot() % QLatin1String("/bin"));
    env.prependOrSetPath(path % QLatin1String("madbin"));

    if (bc->qmakeBuildConfiguration() & QtVersion::DebugBuild) {
        env.appendOrSet(QLatin1String("DEB_BUILD_OPTIONS"),
            QLatin1String("nostrip"), QLatin1String(" "));
    }

    QString perlLib
        = QDir::fromNativeSeparators(path % QLatin1String("madlib/perl5"));
#ifdef Q_OS_WIN
    perlLib = perlLib.remove(QLatin1Char(':'));
    perlLib = perlLib.prepend(QLatin1Char('/'));
#endif
    env.set(QLatin1String("PERL5LIB"), perlLib);
    env.set(QLatin1String("PWD"), workingDir);

    const QRegExp envPattern(QLatin1String("([^=]+)=[\"']?([^;\"']+)[\"']? ;.*"));
    QByteArray line;
    do {
        line = configFile.readLine(200);
        if (envPattern.exactMatch(line))
            env.set(envPattern.cap(1), envPattern.cap(2));
    } while (!line.isEmpty());

    proc->setEnvironment(env.toStringList());
    proc->setWorkingDirectory(workingDir);
    return true;
}

QString MaemoPackageCreationStep::packagingCommand(const MaemoToolChain *tc,
    const QString &commandName)
{
    QString perl;
#ifdef Q_OS_WIN
    perl = tc->maddeRoot() + QLatin1String("/bin/perl.exe ");
#endif
    return perl + tc->maddeRoot() % QLatin1String("/madbin/") % commandName;
}

void MaemoPackageCreationStep::checkProjectName()
{
    const QRegExp legalName(QLatin1String("[0-9-+a-z\\.]+"));
    if (!legalName.exactMatch(buildConfiguration()->target()->project()->displayName())) {
        emit addTask(Task(Task::Warning,
            tr("Your project name contains characters not allowed in Debian packages.\n"
               "They must only use lower-case letters, numbers, '-', '+' and '.'.\n"
               "We will try to work around that, but you may experience problems."),
               QString(), -1, TASK_CATEGORY_BUILDSYSTEM));
    }
}

QString MaemoPackageCreationStep::packageName(const ProjectExplorer::Project *project)
{
    QString packageName = project->displayName().toLower();
    const QRegExp legalLetter(QLatin1String("[a-z0-9+-.]"), Qt::CaseSensitive,
        QRegExp::WildcardUnix);
    for (int i = 0; i < packageName.length(); ++i) {
        if (!legalLetter.exactMatch(packageName.mid(i, 1)))
            packageName[i] = QLatin1Char('-');
    }
    return packageName;
}

QString MaemoPackageCreationStep::packageFileName(const ProjectExplorer::Project *project,
    const QString &version)
{
    return packageName(project) % QLatin1Char('_') % version
        % QLatin1String("_armel.deb");
}

void MaemoPackageCreationStep::updateDesktopFiles(const QString &rulesFilePath)
{
    QFile rulesFile(rulesFilePath);
    if (!rulesFile.open(QIODevice::ReadWrite)) {
        qWarning("Cannot open rules file for Maemo6 icon path adaptation.");
        return;
    }
    QByteArray content = rulesFile.readAll();
    const int makeInstallLine = content.indexOf("\t$(MAKE) INSTALL_ROOT");
    if (makeInstallLine == -1)
        return;
    const int makeInstallEol = content.indexOf('\n', makeInstallLine);
    if (makeInstallEol == -1)
        return;
    QString desktopFileDir = QFileInfo(rulesFile).dir().path()
        + QLatin1Char('/') + projectName()
        + QLatin1String("/usr/share/applications/");
    if (maemoToolChain()->version() == MaemoToolChain::Maemo5)
        desktopFileDir += QLatin1String("hildon/");
#ifdef Q_OS_WIN
    desktopFileDir.remove(QLatin1Char(':'));
    desktopFileDir.prepend(QLatin1Char('/'));
#endif
    int insertPos = makeInstallEol + 1;    
    for (int i = 0; i < deployStep()->deployables()->modelCount(); ++i) {
        const MaemoDeployableListModel * const model
            = deployStep()->deployables()->modelAt(i);
        if (!model->hasDesktopFile())
            continue;
        const QString appName = model->applicationName();
        if (maemoToolChain()->version() == MaemoToolChain::Maemo6) {
            addWorkaroundForHarmattanBug(content, insertPos,
                appName, desktopFileDir);
        }
        const QString executableFilePath = model->remoteExecutableFilePath();
        if (executableFilePath.isEmpty()) {
            qDebug("%s: Skipping subproject %s with missing deployment information.",
                Q_FUNC_INFO, qPrintable(model->proFilePath()));
            continue;
        }
        const QByteArray lineBefore("Exec=.*");
        const QByteArray lineAfter("Exec=" + executableFilePath.toUtf8());
        const QString desktopFilePath
            = desktopFileDir + appName + QLatin1String(".desktop");
        addSedCmdToRulesFile(content, insertPos, desktopFilePath, lineBefore,
            lineAfter);
    }
    rulesFile.resize(0);
    rulesFile.write(content);
}

void MaemoPackageCreationStep::addWorkaroundForHarmattanBug(QByteArray &rulesFileContent,
    int &insertPos, const QString &appName, const QString &desktopFileDir)
{
    const QByteArray lineBefore("Icon=" + appName.toUtf8());
    const QByteArray lineAfter("Icon=/usr/share/icons/hicolor/64x64/apps/"
        + appName.toUtf8() + ".png");
    const QString desktopFilePath
        = desktopFileDir + appName + QLatin1String(".desktop");
    addSedCmdToRulesFile(rulesFileContent, insertPos, desktopFilePath,
        lineBefore, lineAfter);
}

void MaemoPackageCreationStep::addSedCmdToRulesFile(QByteArray &rulesFileContent,
    int &insertPos, const QString &desktopFilePath, const QByteArray &oldString,
    const QByteArray &newString)
{
    const QString tmpFilePath = desktopFilePath + QLatin1String(".sed");
    const QByteArray sedCmd = "\tsed 's:" + oldString + ':' + newString
        + ":' " + desktopFilePath.toLocal8Bit() + " > "
        + tmpFilePath.toLocal8Bit() + " || echo -n\n";
    const QByteArray mvCmd = "\tmv " + tmpFilePath.toLocal8Bit() + ' '
        + desktopFilePath.toLocal8Bit() + " || echo -n\n";
    rulesFileContent.insert(insertPos, sedCmd);
    insertPos += sedCmd.length();
    rulesFileContent.insert(insertPos, mvCmd);
    insertPos += mvCmd.length();
}

const QLatin1String MaemoPackageCreationStep::CreatePackageId("Qt4ProjectManager.MaemoPackageCreationStep");

} // namespace Internal
} // namespace Qt4ProjectManager
