/***************************************************************************
                          midiscriptengine.cpp  -  description
                          -------------------
    begin                : Fri Dec 12 2008
    copyright            : (C) 2008 by Sean M. Pappalardo
                                       "Holy crap, I wrote new code!"
    email                : pegasus@renegadetech.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "controlobject.h"
#include "controlobjectthread.h"
#include "mididevice.h"
#include "midiscriptengine.h"
// #include <QScriptSyntaxCheckResult>

#ifdef _MSC_VER
#include <float.h>  // for _isnan() on VC++
#define isnan(x) _isnan(x)  // VC++ uses _isnan() instead of isnan()
#else
#include <math.h>  // for isnan() everywhere else
#endif


MidiScriptEngine::MidiScriptEngine(MidiDevice* midiDevice) :
    m_pEngine(NULL),
    m_pMidiDevice(midiDevice)
{
}

MidiScriptEngine::~MidiScriptEngine() {

    // Delete the script engine, first clearing the pointer so that
    // other threads will not get the dead pointer after we delete it.
    if(m_pEngine != NULL) {
        QScriptEngine *engine = m_pEngine;
        m_pEngine = NULL;
        engine->deleteLater();
    }

}

/* -------- ------------------------------------------------------
Purpose: Shuts down MIDI scripts in an orderly fashion
            (stops timers then executes shutdown functions)
Input:   -
Output:  -
-------- ------------------------------------------------------ */
void MidiScriptEngine::gracefulShutdown(QList<QString> scriptFunctionPrefixes) {
    qDebug() << "MidiScriptEngine shutting down...";
    m_scriptEngineLock.lock();
    // Clear the m_connectedControls hash so we stop responding
    // to signals.
    m_connectedControls.clear();

    // Disconnect the function call signal
    disconnect(m_pMidiDevice, SIGNAL(callMidiScriptFunction(QString, char, char,
                                                            char, MidiStatusByte, QString)),
                                this, SLOT(execute(QString, char, char, char, MidiStatusByte, QString)));

    // Stop all timers
    stopAllTimers();

    // Call each script's shutdown function if it exists
    QListIterator<QString> prefixIt(scriptFunctionPrefixes);
    while (prefixIt.hasNext()) {
        QString shutName = prefixIt.next();
        if (shutName!="") {
            shutName.append(".shutdown");
            if (m_midiDebug) qDebug() << "MidiScriptEngine: Executing" << shutName;
            if (!internalExecute(shutName))
                qWarning() << "MidiScriptEngine: No" << shutName << "function in script";
        }
    }

    // Free all the control object threads
    QList<ConfigKey> keys = m_controlCache.keys();
    QList<ConfigKey>::iterator it = keys.begin();
    QList<ConfigKey>::iterator end = keys.end();
    while(it != end) {
        ConfigKey key = *it;
        ControlObjectThread *cot = m_controlCache.take(key);
        delete cot;
        it++;
    }

    m_scriptEngineLock.unlock();

    // Stop processing the event loop and terminate the thread.
    quit();
}

bool MidiScriptEngine::isReady() {
    m_scriptEngineLock.lock();
    bool ret = m_pEngine != NULL;
    m_scriptEngineLock.unlock();
    return ret;
}

/*
  WARNING: must hold the lock to call this
 */
void MidiScriptEngine::initializeScriptEngine() {
    // Create the MidiScriptEngine
    m_pEngine = new QScriptEngine(this);

    //qDebug() << "MidiScriptEngine::run() m_pEngine->parent() is " << m_pEngine->parent();
    //qDebug() << "MidiScriptEngine::run() m_pEngine->thread() is " << m_pEngine->thread();

    if (m_pMidiDevice)
        qDebug() << "MIDI Device in script engine is:" << m_pMidiDevice->getName();

    // Make this MidiScriptEngine instance available to scripts as
    // 'engine'.
    QScriptValue engineGlobalObject = m_pEngine->globalObject();
    engineGlobalObject.setProperty("engine", m_pEngine->newQObject(this));

    // Make the MidiDevice instance available to scripts as
    // 'midi'.
    if (m_pMidiDevice)
        engineGlobalObject.setProperty("midi", m_pEngine->newQObject(m_pMidiDevice));

    // Allow the MidiDevice to signal script function calls
    connect(m_pMidiDevice, SIGNAL(callMidiScriptFunction(QString, char, char,
                                                char, MidiStatusByte, QString)),
            this, SLOT(execute(QString, char, char, char, MidiStatusByte, QString)));
}

/* -------- ------------------------------------------------------
   Purpose: Load all script files given in the shared list
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::loadScriptFiles(QList<QString> scriptFileNames) {

    // Set the Midi Debug flag
    if (m_pMidiDevice)
        m_midiDebug = m_pMidiDevice->midiDebugging();

    qDebug() << "MidiScriptEngine: Loading & evaluating all MIDI script code";

    ConfigObject<ConfigValue> *config = new ConfigObject<ConfigValue>(QDir::homePath().append("/").append(SETTINGS_PATH).append(SETTINGS_FILE));

    QString scriptPath = config->getConfigPath().append("midi/");
    delete config;

    QListIterator<QString> it(scriptFileNames);
    m_scriptEngineLock.lock();
    while (it.hasNext()) {
        QString curScriptFileName = it.next();
        safeEvaluate(scriptPath+curScriptFileName);

        if(m_scriptErrors.contains(curScriptFileName)) {
            qDebug() << "Errors occured while loading " << curScriptFileName;
        }
    }

    m_scriptEngineLock.unlock();
    emit(initialized());
}

/* -------- ------------------------------------------------------
   Purpose: Run the initialization function for each loaded script
                if it exists
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::initializeScripts(QList<QString> scriptFunctionPrefixes) {
    m_scriptEngineLock.lock();

    QListIterator<QString> prefixIt(scriptFunctionPrefixes);
    while (prefixIt.hasNext()) {
        QString initName = prefixIt.next();
            if (initName!="") {
                initName.append(".init");
            if (m_midiDebug) qDebug() << "MidiScriptEngine: Executing" << initName;
            if (!safeExecute(initName, m_pMidiDevice->getName()))
                qWarning() << "MidiScriptEngine: No" << initName << "function in script";
        }
    }
    m_scriptEngineLock.unlock();
    emit(initialized());
}

/* -------- ------------------------------------------------------
   Purpose: Create the MidiScriptEngine object (so it is owned in this
   thread, and start the Qt event loop for this thread via exec().
   Input: -
   Output: -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::run() {
    unsigned static id = 0; //the id of this thread, for debugging purposes //XXX copypasta (should factor this out somehow), -kousu 2/2009
    QThread::currentThread()->setObjectName(QString("MidiScriptEngine %1").arg(++id));

    // Prevent the script engine from strangling other parts of Mixxx
    QThread::currentThread()->setPriority(QThread::LowPriority);

    m_scriptEngineLock.lock();
    initializeScriptEngine();
    m_scriptEngineLock.unlock();
    emit(initialized());

    // Run the Qt event loop indefinitely
    exec();
}

/* -------- ------------------------------------------------------
   Purpose: Validate script syntax, then evaluate() it so the
            functions are registered & available for use.
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::evaluate(QString filepath) {
    m_scriptEngineLock.lock();
    bool ret = safeEvaluate(filepath);
    m_scriptEngineLock.unlock();
    return ret;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::execute(QString function) {
    m_scriptEngineLock.lock();
    bool ret = safeExecute(function);
    if (!ret) qDebug() << "MidiScriptEngine: Invalid script function" << function;
    m_scriptEngineLock.unlock();
    return ret;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, data string (e.g. device ID)
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::execute(QString function, QString data) {
    m_scriptEngineLock.lock();
    bool ret = safeExecute(function, data);
    if (!ret) qDebug() << "MidiScriptEngine: Invalid script function" << function;
    m_scriptEngineLock.unlock();
    return ret;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, pointer to data buffer, length of buffer
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::execute(QString function, const unsigned char data[],
                               unsigned int length) {
    m_scriptEngineLock.lock();
    bool ret = safeExecute(function, data, length);
    m_scriptEngineLock.unlock();
    return ret;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, channel #, control #, value, status
                MixxxControl group
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::execute(QString function, char channel,
                               char control, char value,
                               MidiStatusByte status,
                               QString group) {
    m_scriptEngineLock.lock();
    bool ret = safeExecute(function, channel, control, value, status, group);
    if (!ret) qDebug() << "MidiScriptEngine: Invalid script function" << function;
    m_scriptEngineLock.unlock();
    return ret;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::safeExecute(QString function) {
    //qDebug() << QString("MidiScriptEngine: Exec1 Thread ID=%1").arg(QThread::currentThreadId(),0,16);

    if(m_pEngine == NULL)
        return false;

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;

    if (!scriptFunction.isFunction())
        return false;

    scriptFunction.call(QScriptValue());
    if (checkException())
        return false;

    return true;
}


/* -------- ------------------------------------------------------
Purpose: Evaluate & run script code
Input:   Code string
Output:  false if an exception
-------- ------------------------------------------------------ */
bool MidiScriptEngine::internalExecute(QString scriptCode) {
    // A special version of safeExecute since we're evaluating strings, not actual functions
    //  (execute() would print an error that it's not a function every time a timer fires.)
    if(m_pEngine == NULL)
        return false;

    // Check syntax
    QScriptSyntaxCheckResult result = m_pEngine->checkSyntax(scriptCode);
    QString error="";
    switch (result.state()) {
        case (QScriptSyntaxCheckResult::Valid): break;
        case (QScriptSyntaxCheckResult::Intermediate):
            error = "Incomplete code";
            break;
        case (QScriptSyntaxCheckResult::Error):
            error = "Syntax error";
            break;
    }
    if (error!="") {
        error = QString("%1: %2 at line %3, column %4 of script code:\n%5\n")
        .arg(error)
        .arg(result.errorMessage())
        .arg(result.errorLineNumber())
        .arg(result.errorColumnNumber())
        .arg(scriptCode);

        if (m_midiDebug) qCritical() << "MidiScriptEngine:" << error;
        else {
            qDebug() << "MidiScriptEngine:" << error;
            qWarning() << "There was an error in a MIDI script."
                          "\nA control you just used is not working properly and you may experience erratic behavior."
                          "\nCheck the console or mixxx.log file for details.";
        }
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(scriptCode);

    if (checkException())
        return false;

    // If it's not a function, we're done.
    if (!scriptFunction.isFunction())
        return true;

    // If it does happen to be a function, call it.
    scriptFunction.call(QScriptValue());
    if (checkException())
        return false;

    return true;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, data string (e.g. device ID)
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::safeExecute(QString function, QString data) {
    //qDebug() << QString("MidiScriptEngine: Exec2 Thread ID=%1").arg(QThread::currentThreadId(),0,16);

    if(m_pEngine == NULL) {
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;
    if (!scriptFunction.isFunction())
        return false;

    QScriptValueList args;
    args << QScriptValue(m_pEngine, data);

    scriptFunction.call(QScriptValue(), args);
    if (checkException())
        return false;
    return true;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, ponter to data buffer, length of buffer
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::safeExecute(QString function, const unsigned char data[],
                                    unsigned int length) {

    if(m_pEngine == NULL) {
        return false;
    }

    if (!m_pEngine->canEvaluate(function)) {
        qCritical() << "MidiScriptEngine: ?Syntax error in function " << function;
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;
    if (!scriptFunction.isFunction())
        return false;

    QScriptValueList args;
    args << QScriptValue(m_pEngine, (const char*)data);
    args << QScriptValue(m_pEngine, length);

    scriptFunction.call(QScriptValue(), args);
    if (checkException())
        return false;
    return true;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, channel #, control #, value, status
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::safeExecute(QString function, char channel,
                                   char control, char value,
                                   MidiStatusByte status,
                                   QString group) {
    //qDebug() << QString("MidiScriptEngine: Exec2 Thread ID=%1").arg(QThread::currentThreadId(),0,16);

    if(m_pEngine == NULL) {
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;
    if (!scriptFunction.isFunction())
        return false;

    QScriptValueList args;
    args << QScriptValue(m_pEngine, channel);
    args << QScriptValue(m_pEngine, control);
    args << QScriptValue(m_pEngine, value);
    args << QScriptValue(m_pEngine, status);
    args << QScriptValue(m_pEngine, group);

    scriptFunction.call(QScriptValue(), args);
    if (checkException())
        return false;
    return true;
}

/* -------- ------------------------------------------------------
   Purpose: Check to see if a script threw an exception
   Input:   QScriptValue returned from call(scriptFunctionName)
   Output:  true if there was an exception
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::checkException() {
    if(m_pEngine == NULL) {
        return false;
    }

    if (m_pEngine->hasUncaughtException()) {
        QScriptValue exception = m_pEngine->uncaughtException();
        QString errorMessage = exception.toString();
        int line = m_pEngine->uncaughtExceptionLineNumber();
        QStringList backtrace = m_pEngine->uncaughtExceptionBacktrace();
        QString filename = exception.property("fileName").toString();

        QStringList error;
        error << filename << errorMessage << QString(line);
        m_scriptErrors.insert(filename, error);

        if (m_midiDebug)
            qCritical() << "MidiScriptEngine: uncaught exception:"
                        << errorMessage
                        << "in" << filename << "at line"
                        << line
                        << "\nBacktrace:\n"
                        << backtrace;
        else {
            qDebug() << "MidiScriptEngine WARNING: uncaught exception:"
                         << errorMessage
                         << "in" << filename << "at line"
                         << line;
            qWarning() << "There was a problem with a MIDI script."
                          "\nA control you just used is not working properly and you may experience erratic behavior."
                          "\nCheck the console or mixxx.log file for details.";
        }
        return true;
    }
    return false;
}


/* -------- ------------------------------------------------------
   Purpose: Returns a list of functions available in the QtScript
            code
   Input:   -
   Output:  functionList QStringList
   -------- ------------------------------------------------------ */
QStringList MidiScriptEngine::getScriptFunctions() {
    m_scriptEngineLock.lock();
    QStringList ret = m_scriptFunctions;
    m_scriptEngineLock.unlock();
    return ret;
}

void MidiScriptEngine::generateScriptFunctions(QString scriptCode) {

//     QStringList functionList;
    QStringList codeLines = scriptCode.split("\n");

//     qDebug() << "MidiScriptEngine: m_scriptCode=" << m_scriptCode;

    if (m_midiDebug)
        qDebug() << "MidiScriptEngine:" << codeLines.count() << "lines of code being searched for functions";

    // grep 'function' midi/midi-mappings-scripts.js|grep -i '(msg)'|sed -e 's/function \(.*\)(msg).*/\1/i' -e 's/[= ]//g'
    QRegExp rx("*.*function*(*)*");    // Find all lines with function names in them
    rx.setPatternSyntax(QRegExp::Wildcard);

    int position = codeLines.indexOf(rx);

    while (position != -1) {    // While there are more matches

        QString line = codeLines.takeAt(position);    // Pull & remove the current match from the list.

        if (line.indexOf('#') != 0 && line.indexOf("//") != 0) {    // ignore commented out lines
            QStringList field = line.split(" ");
            if (m_midiDebug) qDebug() << "MidiScriptEngine: Found function:" << field[0]
                                      << "at line" << position;
            m_scriptFunctions.append(field[0]);
        }
        position = codeLines.indexOf(rx);
    }

}

ControlObjectThread* MidiScriptEngine::getControlObjectThread(QString group, QString name) {

    ConfigKey key = ConfigKey(group, name);

    ControlObjectThread *cot = NULL;
    if(!m_controlCache.contains(key)) {
        ControlObject *co = ControlObject::getControl(key);
        if(co != NULL) {
            cot = new ControlObjectThread(co);
            m_controlCache.insert(key, cot);
        }
    } else {
        cot = m_controlCache.value(key);
    }

    return cot;

}

/* -------- ------------------------------------------------------
   Purpose: Returns the current value of a Mixxx control (for scripts)
   Input:   Control group (e.g. [Channel1]), Key name (e.g. [filterHigh])
   Output:  The value
   -------- ------------------------------------------------------ */
double MidiScriptEngine::getValue(QString group, QString name) {


    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) {
        m_scriptEngineLock.unlock();
    }

    //qDebug() << QString("----------------------------------MidiScriptEngine: GetValue Thread ID=%1").arg(QThread::currentThreadId(),0,16);

    ControlObjectThread *cot = getControlObjectThread(group, name);
    if (cot == NULL) {
        qDebug() << "MidiScriptEngine: Unknown control" << group << name;
        return 0.0;
    }

    return cot->get();
}

/* -------- ------------------------------------------------------
   Purpose: Sets new value of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::setValue(QString group, QString name, double newValue) {

    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) {
        m_scriptEngineLock.unlock();
    }

    if(isnan(newValue)) {
        qDebug() << "Warning: script setting [" << group << "," << name
                 << "] to NotANumber, ignoring.";
        return;
    }

    //qDebug() << QString("----------------------------------MidiScriptEngine: SetValue Thread ID=%1").arg(QThread::currentThreadId(),0,16);

    ControlObjectThread *cot = getControlObjectThread(group, name);

    if(cot != NULL) {
        cot->slotSet(newValue);
    }

}

/* -------- ------------------------------------------------------
   Purpose: qDebugs script output so it ends up in mixxx.log
   Input:   String to log
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::log(QString message) {

    qDebug()<<message;
}

/* -------- ------------------------------------------------------
   Purpose: Emits valueChanged() so device outputs update
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::trigger(QString group, QString name) {
    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) {
        m_scriptEngineLock.unlock();
    }

    ControlObjectThread *cot = getControlObjectThread(group, name);
    if(cot != NULL) {
        cot->slotSet(cot->get());
    }
}

/* -------- ------------------------------------------------------
   Purpose: (Dis)connects a ControlObject valueChanged() signal to/from a script function
   Input:   Control group (e.g. [Channel1]), Key name (e.g. [filterHigh]),
                script function name, true if you want to disconnect
   Output:  true if successful
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::connectControl(QString group, QString name, QString function, bool disconnect) {
    ControlObject* cobj = ControlObject::getControl(ConfigKey(group,name));

    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) {
        m_scriptEngineLock.unlock();
    }

    //qDebug() << QString("MidiScriptEngine: Connect Thread ID=%1").arg(QThread::currentThreadId(),0,16);


    if(m_pEngine == NULL) {
        return false;
    }

    QScriptValue slot = m_pEngine->evaluate(function);

    if(!checkException() && slot.isFunction()) {
        if(disconnect) {
//             qDebug() << "MidiScriptEngine::connectControl disconnected " << group << name << " from " << function;
            this->disconnect(cobj, SIGNAL(valueChanged(double)),
                             this, SLOT(slotValueChanged(double)));
            this->disconnect(cobj, SIGNAL(valueChangedFromEngine(double)),
                             this, SLOT(slotValueChanged(double)));
            m_connectedControls.remove(cobj->getKey());
        } else {
//             qDebug() << "MidiScriptEngine::connectControl connected " << group << name << " to " << function;
            connect(cobj, SIGNAL(valueChanged(double)),
                    this, SLOT(slotValueChanged(double)),
                    Qt::QueuedConnection);
            connect(cobj, SIGNAL(valueChangedFromEngine(double)),
                    this, SLOT(slotValueChanged(double)),
                    Qt::QueuedConnection);
            m_connectedControls.insert(cobj->getKey(), function);
        }
        return true;
    }

    return false;
}

/* -------- ------------------------------------------------------
   Purpose: Receives valueChanged() slots from ControlObjects, and
   fires off the appropriate script function.
   -------- ------------------------------------------------------ */
void MidiScriptEngine::slotValueChanged(double value) {
    m_scriptEngineLock.lock();

    ControlObject* sender = (ControlObject*)this->sender();
    if(sender == NULL) {
        qDebug() << "MidiScriptEngine::slotValueChanged() Shouldn't happen -- sender == NULL";
        m_scriptEngineLock.unlock();
        return;
    }
    ConfigKey key = sender->getKey();

    //qDebug() << QString("MidiScriptEngine: slotValueChanged Thread ID=%1").arg(QThread::currentThreadId(),0,16);

    if(m_connectedControls.contains(key)) {
        QString function = m_connectedControls.value(key);
//         qDebug() << "MidiScriptEngine::slotValueChanged() received signal from " << key.group << key.item << " ... firing : " << function;

        // Could branch to safeExecute from here, but for now do it this way.
        QScriptValue function_value = m_pEngine->evaluate(function);
        QScriptValueList args;
        args << QScriptValue(m_pEngine, value);
//         function_value.call(QScriptValue(), args);
        args << QScriptValue(m_pEngine, key.group); // Added by Math`
        args << QScriptValue(m_pEngine, key.item);  // Added by Math`
        QScriptValue result = function_value.call(QScriptValue(), args);
        if (result.isError()) {
            qDebug()<< "MidiScriptEngine: Call to " << function << " resulted in an error:  " << result.toString();
        }

    } else {
        qDebug() << "MidiScriptEngine::slotValueChanged() Received signal from ControlObject that is not connected to a script function.";
    }

    m_scriptEngineLock.unlock();
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate a script file
   Input:   Script filename
   Output:  false if the script file has errors or doesn't exist
   -------- ------------------------------------------------------ */
bool MidiScriptEngine::safeEvaluate(QString filename) {

    if(m_pEngine == NULL) {
        return false;
    }

    qDebug() << "MidiScriptEngine: Loading" << filename;

    // Read in the script file
    QFile input(filename);
    if (!input.open(QIODevice::ReadOnly)) {
        qCritical() << "MidiScriptEngine: Problem opening the script file: "
                    << filename
                    << ", error #"
                    << input.error();
        return false;
    }
    QString scriptCode = "";
    scriptCode.append(input.readAll());
    scriptCode.append('\n');
    input.close();

    // Check syntax
    QScriptSyntaxCheckResult result = m_pEngine->checkSyntax(scriptCode);
    QString error="";
    switch (result.state()) {
        case (QScriptSyntaxCheckResult::Valid): break;
        case (QScriptSyntaxCheckResult::Intermediate):
            error = "Incomplete code";
            break;
        case (QScriptSyntaxCheckResult::Error):
            error = "Syntax error";
            break;
    }
    if (error!="") {
        error = QString("%1 at line %2, column %3 in file %4: %5")
                        .arg(error)
                        .arg(result.errorLineNumber())
                        .arg(result.errorColumnNumber())
                        .arg(filename)
                        .arg(result.errorMessage());

        if (m_midiDebug) qCritical() << "MidiScriptEngine:" << error;
        else {
            qDebug() << "MidiScriptEngine:" << error;
            qWarning() << "There was an error in the MIDI script file" << filename
                       << "\nThe functionality provided by this script file will be disabled."
                          "\nCheck the console or mixxx.log file for details.";
        }
        return false;
    }

    // Evaluate the code
    QScriptValue scriptFunction = m_pEngine->evaluate(scriptCode, filename);

    // Record errors
    if(checkException())
        return false;

    // Add the code we evaluated to our index
    generateScriptFunctions(scriptCode);

    return true;
}

/*
 * Check whether a source file that was evaluated()'d has errors.
 */
bool MidiScriptEngine::hasErrors(QString filename) {
    m_scriptEngineLock.lock();
    bool ret = m_scriptErrors.contains(filename);
    m_scriptEngineLock.unlock();
    return ret;
}

/*
 * Get the errors for a source file that was evaluated()'d
 */
const QStringList MidiScriptEngine::getErrors(QString filename) {
    QStringList ret;
    m_scriptEngineLock.lock();
    if(m_scriptErrors.contains(filename))
        ret = m_scriptErrors.value(filename);
    m_scriptEngineLock.unlock();
    return ret;
}


/* -------- ------------------------------------------------------
   Purpose: Creates & starts a timer that runs some script code
                on timeout
   Input:   Number of milliseconds, script function to call,
                whether it should fire just once
   Output:  The timer's ID, 0 if starting it failed
   -------- ------------------------------------------------------ */
int MidiScriptEngine::beginTimer(int interval, QString scriptCode, bool oneShot) {
    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) {
        m_scriptEngineLock.unlock();
    }

    if (interval<20) {
        qDebug() << "Timer request for" << interval << "ms is too short. Setting to the minimum of 20ms.";
        interval=20;
    }
    // This makes use of every QObject's internal timer mechanism. Nice, clean, and simple.
    // See http://doc.trolltech.com/4.6/qobject.html#startTimer for details
    int timerId = startTimer(interval);
    QPair<QString, bool> timerTarget;
    timerTarget.first = scriptCode;
    timerTarget.second = oneShot;
    m_timers[timerId]=timerTarget;
    if (timerId==0) qDebug() << "MIDI Script timer could not be created";
    else if (m_midiDebug) {
        if (oneShot) qDebug() << "Starting one-shot timer:" << timerId;
        else qDebug() << "Starting timer:" << timerId;
    }
    return timerId;
}

/* -------- ------------------------------------------------------
   Purpose: Stops & removes a timer
   Input:   ID of timer to stop
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::stopTimer(int timerId) {
    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) m_scriptEngineLock.unlock();

    if (!m_timers.contains(timerId)) {
        qDebug() << "Killing timer" << timerId << ": That timer does not exist!";
        return;
    }
    if (m_midiDebug) qDebug() << "Killing timer:" << timerId;

    killTimer(timerId);
    m_timers.remove(timerId);
}

/* -------- ------------------------------------------------------
   Purpose: Stops & removes all timers (for shutdown)
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::stopAllTimers() {
    // When this function runs, assert that somebody is holding the script
    // engine lock.
    bool lock = m_scriptEngineLock.tryLock();
    Q_ASSERT(!lock);
    if(lock) m_scriptEngineLock.unlock();

    QMutableHashIterator<int, QPair<QString, bool> > i(m_timers);
    while (i.hasNext()) {
        i.next();
        stopTimer(i.key());
    }
}

/* -------- ------------------------------------------------------
   Purpose: Runs the appropriate script code on timer events
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void MidiScriptEngine::timerEvent(QTimerEvent *event) {
    int timerId = event->timerId();

    m_scriptEngineLock.lock();
    if (!m_timers.contains(timerId)) {
        qDebug() << "Timer" << timerId << "fired but there's no function mapped to it!";
        m_scriptEngineLock.unlock();
        return;
    }

    QPair<QString, bool> timerTarget = m_timers[timerId];
    if (timerTarget.second) stopTimer(timerId);

    internalExecute(timerTarget.first);
    m_scriptEngineLock.unlock();
}
