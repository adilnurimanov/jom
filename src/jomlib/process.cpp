/****************************************************************************
 **
 ** Copyright (C) 2008-2011 Nokia Corporation and/or its subsidiary(-ies).
 ** Contact: Nokia Corporation (info@qt.nokia.com)
 **
 ** This file is part of the jom project on Trolltech Labs.
 **
 ** This file may be used under the terms of the GNU General Public
 ** License version 2.0 or 3.0 as published by the Free Software Foundation
 ** and appearing in the file LICENSE.GPL included in the packaging of
 ** this file.  Please review the following information to ensure GNU
 ** General Public Licensing requirements will be met:
 ** http://www.fsf.org/licensing/licenses/info/GPLv2.html and
 ** http://www.gnu.org/copyleft/gpl.html.
 **
 ** If you are unsure which license is appropriate for your use, please
 ** contact the sales department at qt-sales@nokia.com.
 **
 ** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 ** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 **
 ****************************************************************************/

#include "process.h"
#include "iocompletionport.h"

#include <QByteArray>
#include <QDir>
#include <QMap>
#include <QMutex>
#include <QTimer>

#include <qt_windows.h>

namespace NMakeFile {

Q_GLOBAL_STATIC(IoCompletionPort, iocp)

struct Pipe
{
    Pipe()
        : hWrite(INVALID_HANDLE_VALUE)
        , hRead(INVALID_HANDLE_VALUE)
    {
        ZeroMemory(&overlapped, sizeof(overlapped));
    }

    ~Pipe()
    {
        if (hWrite != INVALID_HANDLE_VALUE)
            CloseHandle(hWrite);
        if (hRead != INVALID_HANDLE_VALUE)
            CloseHandle(hRead);
    }

    HANDLE hWrite;
    HANDLE hRead;
    OVERLAPPED overlapped;
};

static void safelyCloseHandle(HANDLE &h)
{
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
}

class ProcessPrivate : public IoCompletionPortObserver
{
public:
    ProcessPrivate(Process *process)
        : q(process),
          hProcess(INVALID_HANDLE_VALUE),
          hProcessThread(INVALID_HANDLE_VALUE),
          exitCode(STILL_ACTIVE)
    {
    }

    bool startRead();
    void completionPortNotified(DWORD numberOfBytes, DWORD errorCode);

    Process *q;
    HANDLE hProcess;
    HANDLE hProcessThread;
    Pipe stdoutPipe;
    Pipe stderrPipe;
    Pipe stdinPipe;     // we don't use it but some processes demand it (e.g. xcopy)
    QByteArray outputBuffer;
    QMutex outputBufferLock;
    QMutex bufferedOutputModeSwitchMutex;
    QByteArray intermediateOutputBuffer;
    DWORD exitCode;
};

Process::Process(QObject *parent)
    : QObject(parent),
      d(new ProcessPrivate(this)),
      m_state(NotRunning),
      m_exitCode(0),
      m_exitStatus(NormalExit),
      m_bufferedOutput(true)
{
    static bool metaTypesRegistered = false;
    if (!metaTypesRegistered) {
        metaTypesRegistered = true;
        qRegisterMetaType<ExitStatus>("Process::ExitStatus");
        qRegisterMetaType<ProcessError>("Process::ProcessError");
        qRegisterMetaType<ProcessState>("Process::ProcessState");
    }
}

Process::~Process()
{
    if (m_state == Running)
        qWarning("Process: destroyed while process still running.");
    printBufferedOutput();
    delete d;
}

void Process::setBufferedOutput(bool b)
{
    if (m_bufferedOutput == b)
        return;

    d->bufferedOutputModeSwitchMutex.lock();

    m_bufferedOutput = b;
    if (!m_bufferedOutput)
        printBufferedOutput();

    d->bufferedOutputModeSwitchMutex.unlock();
}

void Process::setWorkingDirectory(const QString &path)
{
    m_workingDirectory = path;
}

static QByteArray createEnvBlock(const QMap<QString,QString> &environment,
                                 const QString &pathKey, const QString &rootKey)
{
    QByteArray envlist;
    if (!environment.isEmpty()) {
        QMap<QString,QString> copy = environment;

        // add PATH if necessary (for DLL loading)
        if (!copy.contains(pathKey)) {
            QByteArray path = qgetenv("PATH");
            if (!path.isEmpty())
                copy.insert(pathKey, QString::fromLocal8Bit(path));
        }

        // add systemroot if needed
        if (!copy.contains(rootKey)) {
            QByteArray systemRoot = qgetenv("SystemRoot");
            if (!systemRoot.isEmpty())
                copy.insert(rootKey, QString::fromLocal8Bit(systemRoot));
        }

        int pos = 0;
        QMap<QString,QString>::ConstIterator it = copy.constBegin(),
                                             end = copy.constEnd();

        static const wchar_t equal = L'=';
        static const wchar_t nul = L'\0';

        for ( ; it != end; ++it) {
            uint tmpSize = sizeof(wchar_t) * (it.key().length() + it.value().length() + 2);
            // ignore empty strings
            if (tmpSize == sizeof(wchar_t) * 2)
                continue;
            envlist.resize(envlist.size() + tmpSize);

            tmpSize = it.key().length() * sizeof(wchar_t);
            memcpy(envlist.data()+pos, it.key().utf16(), tmpSize);
            pos += tmpSize;

            memcpy(envlist.data()+pos, &equal, sizeof(wchar_t));
            pos += sizeof(wchar_t);

            tmpSize = it.value().length() * sizeof(wchar_t);
            memcpy(envlist.data()+pos, it.value().utf16(), tmpSize);
            pos += tmpSize;

            memcpy(envlist.data()+pos, &nul, sizeof(wchar_t));
            pos += sizeof(wchar_t);
        }
        // add the 2 terminating 0 (actually 4, just to be on the safe side)
        envlist.resize( envlist.size()+4 );
        envlist[pos++] = 0;
        envlist[pos++] = 0;
        envlist[pos++] = 0;
        envlist[pos++] = 0;
    }
    return envlist;
}

void Process::setEnvironment(const QStringList &environment)
{
    m_environment = environment;

    QMap<QString,QString> envmap;
    QString pathKey(QLatin1String("Path")), rootKey(QLatin1String("SystemRoot"));
    foreach (const QString &str, m_environment) {
        int idx = str.indexOf(QLatin1Char('='));
        if (idx < 0)
            continue;
        QString name = str.left(idx);
        QString upperName = name.toUpper();
        if (upperName == QLatin1String("PATH"))
            pathKey = name;
        else if (upperName == QLatin1String("SYSTEMROOT"))
            rootKey = name;
        QString value = str.mid(idx + 1);
        envmap.insert(name, value);
    }

    m_envBlock = createEnvBlock(envmap, pathKey, rootKey);
}

enum PipeType { InputPipe, OutputPipe };

static bool setupPipe(Pipe &pipe, SECURITY_ATTRIBUTES *sa, PipeType pt)
{
    BOOL oldInheritHandle = sa->bInheritHandle;
    static DWORD instanceCount = 0;
    const size_t maxPipeNameLen = 256;
    wchar_t pipeName[maxPipeNameLen];
    swprintf_s(pipeName, maxPipeNameLen, L"\\\\.\\pipe\\jom-%X-%X",
               GetCurrentProcessId(), instanceCount++);

    sa->bInheritHandle = (pt == InputPipe);
    const DWORD dwPipeBufferSize = 1024 * 1024;
    HANDLE hRead;
    hRead = CreateNamedPipe(pipeName,
                            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            1,                      // only one pipe instance
                            0,                      // output buffer size
                            dwPipeBufferSize,       // input buffer size
                            0,
                            sa);
    if (hRead == INVALID_HANDLE_VALUE) {
        qErrnoWarning("Process: CreateNamedPipe failed.");
        return false;
    }

    sa->bInheritHandle = (pt == OutputPipe);
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    hWrite = CreateFile(pipeName,
                        GENERIC_WRITE,
                        0,
                        sa,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);
    if (hWrite == INVALID_HANDLE_VALUE) {
        qErrnoWarning("Process: CreateFile failed.");
        CloseHandle(hRead);
        return false;
    }

    // Wait until connection is in place.
    ConnectNamedPipe(hRead, NULL);

    pipe.hRead = hRead;
    pipe.hWrite = hWrite;
    sa->bInheritHandle = oldInheritHandle;
    return true;
}

void Process::start(const QString &commandLine)
{
    m_state = Starting;

    SECURITY_ATTRIBUTES sa = {0};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!setupPipe(d->stdinPipe, &sa, InputPipe))
        qFatal("Cannot setup pipe for stdin.");
    if (!setupPipe(d->stdoutPipe, &sa, OutputPipe))
        qFatal("Cannot setup pipe for stdout.");

    // Let the child process write to the same handle, like QProcess::MergedChannels.
    DuplicateHandle(GetCurrentProcess(), d->stdoutPipe.hWrite, GetCurrentProcess(),
                    &d->stderrPipe.hWrite, 0, TRUE, DUPLICATE_SAME_ACCESS);

    STARTUPINFO si = {0};
    si.cb = sizeof(si);
    si.hStdInput = d->stdinPipe.hRead;
    si.hStdOutput = d->stdoutPipe.hWrite;
    si.hStdError = d->stderrPipe.hWrite;
    si.dwFlags = STARTF_USESTDHANDLES;

    DWORD dwCreationFlags = CREATE_UNICODE_ENVIRONMENT;
    PROCESS_INFORMATION pi;
    wchar_t *strCommandLine = _wcsdup(commandLine.utf16());     // CreateProcess can modify this string
    const wchar_t *strWorkingDir = 0;
    if (!m_workingDirectory.isEmpty()) {
        m_workingDirectory = QDir::toNativeSeparators(m_workingDirectory);
        strWorkingDir = m_workingDirectory.utf16();
    }
    void *envBlock = (m_envBlock.isEmpty() ? 0 : m_envBlock.data());
    BOOL bResult = CreateProcess(NULL, strCommandLine,
                                 0, 0, TRUE, dwCreationFlags, envBlock,
                                 strWorkingDir, &si, &pi);
    free(strCommandLine);
    strCommandLine = 0;
    if (!bResult) {
        m_state = NotRunning;
        emit error(FailedToStart);
        return;
    }

    // Close the pipe handles. This process doesn't need them anymore.
    safelyCloseHandle(d->stdinPipe.hRead);
    safelyCloseHandle(d->stdinPipe.hWrite);
    safelyCloseHandle(d->stdoutPipe.hWrite);
    safelyCloseHandle(d->stderrPipe.hWrite);

    d->hProcess = pi.hProcess;
    d->hProcessThread = pi.hThread;
    iocp()->registerObserver(d, d->stdoutPipe.hRead);
    if (d->startRead()) {
        m_state = Running;
    } else {
        emit error(FailedToStart);
    }
}

void Process::onProcessFinished()
{
    if (m_state != Running)
        return;

    iocp()->unregisterObserver(d);
    safelyCloseHandle(d->stdoutPipe.hRead);
    safelyCloseHandle(d->stderrPipe.hRead);
    safelyCloseHandle(d->hProcess);
    safelyCloseHandle(d->hProcessThread);
    printBufferedOutput();
    m_state = NotRunning;
    DWORD exitCode = d->exitCode;
    d->exitCode = STILL_ACTIVE;

    //### for now we assume a crash if exit code is less than -1 or the magic number
    bool crashed = (exitCode == 0xf291 || (int)exitCode < 0);
    ExitStatus exitStatus = crashed ? Process::CrashExit : Process::NormalExit;
    emit finished(exitCode, exitStatus);
}

bool Process::waitForFinished()
{
    if (m_state != Running)
        return true;
    //if (WaitForSingleObject(d->hProcess, INFINITE) == WAIT_TIMEOUT)
    //    return false;

    QEventLoop eventLoop;
    connect(this, SIGNAL(finished(int, Process::ExitStatus)), &eventLoop, SLOT(quit()));
    eventLoop.exec();

    m_state = NotRunning;
    return true;
}

/**
 * Starts the asynchronous read operation.
 * Returns true, if initiating the read operation was successful.
 */
bool ProcessPrivate::startRead()
{
    DWORD dwRead;
    BOOL bSuccess;

    const DWORD minReadBufferSize = 4096;
    bSuccess = PeekNamedPipe(stdoutPipe.hRead, NULL, 0, NULL, &dwRead, NULL);
    if (!bSuccess || dwRead < minReadBufferSize)
        dwRead = minReadBufferSize;

    intermediateOutputBuffer.resize(dwRead);
    bSuccess = ReadFile(stdoutPipe.hRead,
                        intermediateOutputBuffer.data(),
                        intermediateOutputBuffer.size(),
                        NULL,
                        &stdoutPipe.overlapped);
    if (!bSuccess) {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_IO_PENDING)
            return false;
    }
    return true;
}

void Process::printBufferedOutput()
{
    d->outputBufferLock.lock();
    if (!d->outputBuffer.isEmpty()) {
        fputs(d->outputBuffer.data(), stdout);
        fflush(stdout);
        d->outputBuffer.clear();
    }
    d->outputBufferLock.unlock();
}

void ProcessPrivate::completionPortNotified(DWORD numberOfBytes, DWORD errorCode)
{
    if (numberOfBytes)  {
        bufferedOutputModeSwitchMutex.lock();

        if (q->m_bufferedOutput) {
            outputBufferLock.lock();
            outputBuffer.append(intermediateOutputBuffer.data(), numberOfBytes);
            outputBufferLock.unlock();
        } else {
            intermediateOutputBuffer[(uint)numberOfBytes] = 0;
            printf(intermediateOutputBuffer.data());
            fflush(stdout);
        }

        bufferedOutputModeSwitchMutex.unlock();
    }

    if (errorCode == ERROR_SUCCESS)
        if (startRead())
            return;

    if (exitCode == STILL_ACTIVE)
        if (!GetExitCodeProcess(hProcess, &exitCode))
            exitCode = STILL_ACTIVE;

    if (exitCode != STILL_ACTIVE)
        QTimer::singleShot(0, q, SLOT(onProcessFinished()));
}

} // namespace NMakeFile
