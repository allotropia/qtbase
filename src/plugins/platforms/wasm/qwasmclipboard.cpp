/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <mutex>

#include "qwasmclipboard.h"
#include "qwasmwindow.h"
#include "qwasmstring.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/bind.h>
#include <emscripten/threading.h>

#include <QCoreApplication>
#include <qpa/qwindowsysteminterface.h>

using namespace emscripten;

// there has got to be a better way...
static std::mutex g_clipboardMutex;
static QString g_clipboardText;
static QString g_clipboardFormat;

static val getClipboardData()
{
    //TODO: For correctness, both g_clipboardText and g_clipboardFormat in getClipboardData and
    // getClipboardFormat should atomically be read under a single g_clipboardMutex lock:
    std::scoped_lock g(g_clipboardMutex);
    return QWasmString::fromQString(g_clipboardText);
}

static val getClipboardFormat()
{
    //TODO: For correctness, both g_clipboardText and g_clipboardFormat in getClipboardData and
    // getClipboardFormat should atomically be read under a single g_clipboardMutex lock:
    std::scoped_lock g(g_clipboardMutex);
    return QWasmString::fromQString(g_clipboardFormat);
}

static void pasteClipboardData(QString format, emscripten::val dataPtr)
{
    QString formatString = format;
    QByteArray dataArray =  QByteArray::fromStdString(dataPtr.as<std::string>());
    QMimeData *mMimeData = new QMimeData;
    mMimeData->setData(formatString, dataArray);
    QWasmClipboard::qWasmClipboardPaste(mMimeData);
}

static void qClipboardPromiseResolve(emscripten::val something)
{
    pasteClipboardData("text/plain", something);
}

static void qClipboardCutTo(val event)
{
    if (!QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi) {
        // Send synthetic Ctrl+X to make the app cut data to Qt's clipboard
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
            0, QEvent::KeyPress, Qt::Key_X,  Qt::ControlModifier, "X");
    }

    val module = val::global("Module");
    val clipdata = module.call<val>("qtGetClipboardData");
    val clipFormat = module.call<val>("qtGetClipboardFormat");
    event["clipboardData"].call<void>("setData", clipFormat, clipdata);
    event.call<void>("preventDefault");
}

static void qClipboardCopyTo(val event)
{
    if (!QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi) {
        // Send synthetic Ctrl+C to make the app copy data to Qt's clipboard
        QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
            0, QEvent::KeyPress, Qt::Key_C,  Qt::ControlModifier, "C");
    }

    val module = val::global("Module");
    val clipdata = module.call<val>("qtGetClipboardData");
    val clipFormat = module.call<val>("qtGetClipboardFormat");
    event["clipboardData"].call<void>("setData", clipFormat, clipdata);
    event.call<void>("preventDefault");
}

static void qClipboardPasteTo(val event)
{
    bool hasClipboardApi = QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi;
    val clipdata = hasClipboardApi ?
        val::global("Module").call<val>("qtGetClipboardData") :
        event["clipboardData"].call<val>("getData", val("text"));

    const QString qstr = QWasmString::toQString(clipdata);
    if (qstr.length() > 0) {
        QMimeData *mMimeData = new QMimeData;
        mMimeData->setText(qstr);
        QWasmClipboard::qWasmClipboardPaste(mMimeData);
    }
}

EMSCRIPTEN_BINDINGS(qtClipboardModule) {
    function("qtGetClipboardData", &getClipboardData);
    function("qtGetClipboardFormat", &getClipboardFormat);
    function("qtClipboardPromiseResolve", &qClipboardPromiseResolve);
    function("qtClipboardCutTo", &qClipboardCutTo);
    function("qtClipboardCopyTo", &qClipboardCopyTo);
    function("qtClipboardPasteTo", &qClipboardPasteTo);
}

extern "C" {
static bool setUp() {
    val clipboard = val::global("navigator")["clipboard"];
    val permissions = val::global("navigator")["permissions"];
    if (clipboard.isUndefined() || permissions.isUndefined() || clipboard["readText"].isUndefined())
    {
        return false;
    }

    val readPermissionsMap = val::object();
    readPermissionsMap.set("name", val("clipboard-read"));
    permissions.call<val>("query", readPermissionsMap);

    val writePermissionsMap = val::object();
    writePermissionsMap.set("name", val("clipboard-write"));
    permissions.call<val>("query", writePermissionsMap);

    return true;
}
}

QWasmClipboard::QWasmClipboard()
{
    hasClipboardApi = emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_I, setUp);
}

QWasmClipboard::~QWasmClipboard()
{
    std::scoped_lock g(g_clipboardMutex);
    g_clipboardText.clear();
    g_clipboardFormat.clear();
}

QMimeData* QWasmClipboard::mimeData(QClipboard::Mode mode)
{
    if (mode != QClipboard::Clipboard)
        return nullptr;

    return QPlatformClipboard::mimeData(mode);
}

void QWasmClipboard::setMimeData(QMimeData* mimeData, QClipboard::Mode mode)
{
    if (mimeData->hasText()) {
        QString format = mimeData->formats().at(0);
        QString text = mimeData->text();
        {
            std::scoped_lock g(g_clipboardMutex);
            g_clipboardFormat = format;
            g_clipboardText = text;
        }
    } else if (mimeData->hasHtml()) {
        QString format = mimeData->formats().at(0);
        QString text = mimeData->html();
        {
            std::scoped_lock g(g_clipboardMutex);
            g_clipboardFormat = format;
            g_clipboardText = text;
        }
    }

    QPlatformClipboard::setMimeData(mimeData, mode);
}

bool QWasmClipboard::supportsMode(QClipboard::Mode mode) const
{
    return mode == QClipboard::Clipboard;
}

bool QWasmClipboard::ownsMode(QClipboard::Mode mode) const
{
    Q_UNUSED(mode);
    return false;
}

void QWasmClipboard::qWasmClipboardPaste(QMimeData *mData)
{
    QWasmIntegration::get()->clipboard()->setMimeData(mData, QClipboard::Clipboard);

    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(
                0, QEvent::KeyPress, Qt::Key_V,  Qt::ControlModifier, "V");
}

void QWasmClipboard::installEventHandlers(const emscripten::val &canvas)
{
    if (hasClipboardApi)
        return;

    // Fallback path for browsers which do not support direct clipboard access
    canvas.call<void>("addEventListener", val("cut"),
                      val::module_property("qtClipboardCutTo"));
    canvas.call<void>("addEventListener", val("copy"),
                      val::module_property("qtClipboardCopyTo"));
    canvas.call<void>("addEventListener", val("paste"),
                      val::module_property("qtClipboardPasteTo"));
}

extern "C" {
static void doReadTextFromClipboard() {
    val navigator = val::global("navigator");
    val textPromise = navigator["clipboard"].call<val>("readText");
    val readTextResolve = val::global("Module")["qtClipboardPromiseResolve"];
    textPromise.call<val>("then", readTextResolve);
}
}

void QWasmClipboard::readTextFromClipboard()
{
    if (QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi) {
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, doReadTextFromClipboard);;
    }
}

extern "C" {
static void doWriteTextToClipboard() {
    QString text;
    {
        std::scoped_lock g(g_clipboardMutex);
        text = g_clipboardText;
    }
    val txt = QWasmString::fromQString(text);
    val navigator = val::global("navigator");
    navigator["clipboard"].call<void>("writeText", txt);
}
}

void QWasmClipboard::writeTextToClipboard()
{
    if (QWasmIntegration::get()->getWasmClipboard()->hasClipboardApi) {
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, doWriteTextToClipboard);;
    }
}
