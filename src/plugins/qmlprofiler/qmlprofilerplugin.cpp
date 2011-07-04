/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2011 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (info@qt.nokia.com)
**
** No Commercial Usage
**
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
**
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
** Nokia at info@qt.nokia.com.
**
**************************************************************************/

#include "qmlprofilerplugin.h"

#include "qmlprofilertool.h"
#include "qmlprojectanalyzerruncontrolfactory.h"

#include <analyzerbase/analyzermanager.h>

#include <QtCore/QtPlugin>

using namespace Analyzer;
using namespace QmlProfiler::Internal;

bool QmlProfilerPlugin::debugOutput = false;

bool QmlProfilerPlugin::initialize(const QStringList &arguments, QString *errorString)
{
    Q_UNUSED(arguments)
    Q_UNUSED(errorString)
    addAutoReleasedObject(new QmlProjectAnalyzerRunControlFactory());
    StartModes modes;
    // They are handled the same actually.
    //modes.append(StartMode(StartRemote));
    modes.append(StartMode(StartLocal));
    AnalyzerManager::addTool(new QmlProfilerTool(this), modes);
    return true;
}

void QmlProfilerPlugin::extensionsInitialized()
{
    // Retrieve objects from the plugin manager's object pool.
    // "In the extensionsInitialized method, a plugin can be sure that all
    //  plugins that depend on it are completely initialized."
}

ExtensionSystem::IPlugin::ShutdownFlag QmlProfilerPlugin::aboutToShutdown()
{
    // Save settings.
    // Disconnect from signals that are not needed during shutdown
    // Hide UI (if you add UI that is not in the main window directly)
    return SynchronousShutdown;
}

Q_EXPORT_PLUGIN(QmlProfilerPlugin)

