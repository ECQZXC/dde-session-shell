/*
 * Copyright (C) 2015 ~ 2018 Deepin Technology Co., Ltd.
 *
 * Author:     sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
 *
 * Maintainer: sbw <sbw@sbw.so>
 *             kirigaya <kirigaya@mkacg.com>
 *             Hualet <mr.asianwang@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "warningcontent.h"

WarningContent::WarningContent(SessionBaseModel * const model, const SessionBaseModel::PowerAction action, QWidget *parent)
    : SessionBaseWindow(parent)
    , m_model(model)
    , m_login1Inter(new DBusLogin1Manager("org.freedesktop.login1", "/org/freedesktop/login1", QDBusConnection::systemBus(), this))
    , m_powerAction(action)
{
    m_inhibitorBlacklists << "NetworkManager" << "ModemManager" << "com.deepin.daemon.Power";
    setTopFrameVisible(false);
    setBottomFrameVisible(false);
}

WarningContent::~WarningContent()
{

}

QList<InhibitWarnView::InhibitorData> WarningContent::listInhibitors(const SessionBaseModel::PowerAction action)
{
    QList<InhibitWarnView::InhibitorData> inhibitorList;

    if (m_login1Inter->isValid()) {
        qDebug() <<  "m_login1Inter is valid!";

        QDBusPendingReply<InhibitorsList> reply = m_login1Inter->ListInhibitors();
        reply.waitForFinished();

        if (!reply.isError()) {
            InhibitorsList inhibitList = qdbus_cast<InhibitorsList>(reply.argumentAt(0));
            qDebug() << "inhibitList:" << inhibitList.count();

            QString type;

            switch (action) {
            case SessionBaseModel::PowerAction::RequireShutdown:
            case SessionBaseModel::PowerAction::RequireRestart:
            case SessionBaseModel::PowerAction::RequireSwitchSystem:
            case SessionBaseModel::PowerAction::RequireLogout:
                type = "shutdown";
                break;
            case SessionBaseModel::PowerAction::RequireSuspend:
            case SessionBaseModel::PowerAction::RequireHibernate:
                type = "sleep";
                break;
            default:
                return {};
            }

            for (int i = 0; i < inhibitList.count(); i++) {
                // Just take care of DStore's inhibition, ignore others'.
                const Inhibit &inhibitor = inhibitList.at(i);
                if (inhibitor.what.split(':', QString::SkipEmptyParts).contains(type)
                        && !m_inhibitorBlacklists.contains(inhibitor.who)) {

                    // 待机时，非block暂不处理，因为目前没有倒计时待机功能
                    if (type == "sleep" && inhibitor.mode != "block")
                        continue;

                    InhibitWarnView::InhibitorData inhibitData;
                    inhibitData.who = inhibitor.who;
                    inhibitData.why = inhibitor.why;
                    inhibitData.mode = inhibitor.mode;
                    inhibitData.pid = inhibitor.pid;

                    if(action == SessionBaseModel::PowerAction::RequireLogout && inhibitor.uid != m_model->currentUser()->uid())
                        continue;

                    // 读取翻译后的文本，读取应用图标
                    QDBusConnection connection = QDBusConnection::sessionBus();
                    if (!inhibitor.uid) {
                        connection = QDBusConnection::systemBus();
                    }

                    if (connection.interface()->isServiceRegistered(inhibitor.who)) {

                        QDBusInterface ifc(inhibitor.who, "/com/deepin/InhibitHint", "com.deepin.InhibitHint", connection);
                        QDBusMessage msg = ifc.call("Get", qgetenv("LANG"), inhibitor.why);
                        if (msg.type() == QDBusMessage::ReplyMessage) {
                            InhibitHint inhibitHint = qdbus_cast<InhibitHint>(msg.arguments().at(0).value<QDBusArgument>());

                            if (!inhibitHint.why.isEmpty()) {
                                inhibitData.who = inhibitHint.name;
                                inhibitData.why = inhibitHint.why;
                                inhibitData.icon = inhibitHint.icon;
                            }
                        }
                    }

                    inhibitorList.append(inhibitData);
                }
            }

            qDebug() << "List of valid '" << type << "' inhibitors:";

            for (const InhibitWarnView::InhibitorData &data : inhibitorList) {
                qDebug() << "who:" << data.who;
                qDebug() << "why:" << data.why;
                qDebug() << "pid:" << data.pid;
            }

            qDebug() << "End list inhibitor";
        } else {
            qWarning() << "D-Bus request reply error:" << reply.error().message();
        }
    } else {
        qWarning() << "shutdown login1Manager error!";
    }

    return inhibitorList;
}

void WarningContent::doCancelShutdownInhibit()
{
    if (m_model->isCheckedInhibit()) return;

    m_model->setIsCheckedInhibit(true);
    m_model->setPowerAction(SessionBaseModel::PowerAction::None);
    emit m_model->cancelShutdownInhibit();
}

void WarningContent::doAccecpShutdownInhibit()
{
    if (m_model->isCheckedInhibit()) return;

    m_model->setIsCheckedInhibit(true);
    m_model->setPowerAction(m_powerAction, true);
    if (m_model->currentModeState() != SessionBaseModel::ModeStatus::ShutDownMode)
        emit m_model->cancelShutdownInhibit();
}

void WarningContent::beforeInvokeAction(bool needConfirm)
{
    const QList<InhibitWarnView::InhibitorData> inhibitors = listInhibitors(m_powerAction);
    const QList<std::shared_ptr<User>> &loginUsers = m_model->loginedUserList();

    if (m_warningView != nullptr) {
        m_warningView->deleteLater();
        m_warningView = nullptr;
    }

    // change ui
    if (!inhibitors.isEmpty()) {
        InhibitWarnView *view = new InhibitWarnView(m_powerAction, this);
        view->setFocusPolicy(Qt::StrongFocus);
        setFocusPolicy(Qt::NoFocus);
        view->setInhibitorList(inhibitors);

        switch (m_powerAction) {
        case SessionBaseModel::PowerAction::RequireShutdown:
            view->setInhibitConfirmMessage(tr("The programs are preventing the computer from shutting down, and forcing shut down may cause data loss.") + "\n" +
                                           tr("To close the program, click Cancel, and then close the program."));
            break;
        case SessionBaseModel::PowerAction::RequireSwitchSystem:
        case SessionBaseModel::PowerAction::RequireRestart:
            view->setInhibitConfirmMessage(tr("The programs are preventing the computer from reboot, and forcing reboot may cause data loss.") + "\n" +
                                           tr("To close the program, click Cancel, and then close the program."));
            break;
        case SessionBaseModel::PowerAction::RequireSuspend:
            view->setInhibitConfirmMessage(tr("The programs are preventing the computer from suspend, and forcing suspend may cause data loss.") + "\n" +
                                           tr("To close the program, click Cancel, and then close the program."));
            break;
        case SessionBaseModel::PowerAction::RequireHibernate:
            view->setInhibitConfirmMessage(tr("The programs are preventing the computer from hibernate, and forcing hibernate may cause data loss.") + "\n" +
                                           tr("To close the program, click Cancel, and then close the program."));
            break;
        case SessionBaseModel::PowerAction::RequireLogout:
            view->setInhibitConfirmMessage(tr("The programs are preventing the computer from log out, and forcing log out may cause data loss.") + "\n" +
                                           tr("To close the program, click Cancel, and then close the program."));
            break;
        default:
            return;
        }

        bool isAccept = true;
        for (auto inhib : inhibitors) {
            if (inhib.mode.compare("block") == 0) {
                isAccept = false;
                break;
            }
        }

        if (m_powerAction == SessionBaseModel::PowerAction::RequireShutdown) {
            view->setAcceptReason(tr("Shut down"));
            view->setAcceptVisible(isAccept);
        } else if (m_powerAction == SessionBaseModel::PowerAction::RequireRestart || m_powerAction == SessionBaseModel::PowerAction::RequireSwitchSystem) {
            view->setAcceptReason(tr("Reboot"));
            view->setAcceptVisible(isAccept);
        } else if (m_powerAction == SessionBaseModel::PowerAction::RequireSuspend) {
            view->setAcceptReason(tr("Suspend"));
            view->setAcceptVisible(isAccept);
        } else if (m_powerAction == SessionBaseModel::PowerAction::RequireHibernate) {
            view->setAcceptReason(tr("Hibernate"));
            view->setAcceptVisible(isAccept);
        } else if (m_powerAction == SessionBaseModel::PowerAction::RequireLogout) {
            view->setAcceptReason(tr("Log out"));
            view->setAcceptVisible(isAccept);
        }

        m_warningView = view;
        m_warningView->setFixedSize(getCenterContentSize());
        setCenterContent(m_warningView);

        connect(view, &InhibitWarnView::cancelled, this, &WarningContent::doCancelShutdownInhibit);
        connect(view, &InhibitWarnView::actionInvoked, this, &WarningContent::doAccecpShutdownInhibit);

        return;
    }

    if (loginUsers.length() > 1 && (m_powerAction == SessionBaseModel::PowerAction::RequireShutdown || m_powerAction == SessionBaseModel::PowerAction::RequireRestart)) {
        QList<std::shared_ptr<User>> tmpList = loginUsers;

        for (auto it = tmpList.begin(); it != tmpList.end();) {
            if ((*it)->uid() == m_model->currentUser()->uid()) {
                it = tmpList.erase(it);
                continue;
            }
            ++it;
        }

        MultiUsersWarningView *view = new MultiUsersWarningView(m_powerAction, this);
        view->setFocusPolicy(Qt::StrongFocus);
        setFocusPolicy(Qt::NoFocus);
        view->setUsers(tmpList);
        if (m_powerAction == SessionBaseModel::PowerAction::RequireShutdown)
            view->setAcceptReason(tr("Shut down"));
        else if (m_powerAction == SessionBaseModel::PowerAction::RequireRestart)
            view->setAcceptReason(tr("Reboot"));

        m_warningView = view;
        m_warningView->setFixedSize(getCenterContentSize());
        setCenterContent(m_warningView);

        connect(view, &MultiUsersWarningView::cancelled, this, &WarningContent::doCancelShutdownInhibit);
        connect(view, &MultiUsersWarningView::actionInvoked, this, &WarningContent::doAccecpShutdownInhibit);

        return;
    }

    if (needConfirm && (m_powerAction == SessionBaseModel::PowerAction::RequireShutdown ||
                        m_powerAction == SessionBaseModel::PowerAction::RequireRestart ||
                        m_powerAction == SessionBaseModel::PowerAction::RequireLogout)) {
        InhibitWarnView *view = new InhibitWarnView(m_powerAction, this);
        view->setFocusPolicy(Qt::StrongFocus);
        setFocusPolicy(Qt::NoFocus);
        if (m_powerAction == SessionBaseModel::PowerAction::RequireShutdown) {
            view->setAcceptReason(tr("Shut down"));
            view->setInhibitConfirmMessage(tr("Are you sure you want to shut down?"));
        } else if (m_powerAction == SessionBaseModel::PowerAction::RequireRestart) {
            view->setAcceptReason(tr("Reboot"));
            view->setInhibitConfirmMessage(tr("Are you sure you want to reboot?"));
        } else if (m_powerAction == SessionBaseModel::PowerAction::RequireLogout) {
            view->setAcceptReason(tr("Log out"));
            view->setInhibitConfirmMessage(tr("Are you sure you want to log out?"));
        }

        m_warningView = view;
        m_warningView->setFixedSize(getCenterContentSize());
        setCenterContent(m_warningView);

        connect(view, &InhibitWarnView::cancelled, this, &WarningContent::doCancelShutdownInhibit);
        connect(view, &InhibitWarnView::actionInvoked, this, &WarningContent::doAccecpShutdownInhibit);

        return;
    }

    doAccecpShutdownInhibit();
}

void WarningContent::setPowerAction(const SessionBaseModel::PowerAction action)
{
    if (m_powerAction == action)
        return;
    m_powerAction = action;
}

void WarningContent::mouseReleaseEvent(QMouseEvent *event)
{
    doCancelShutdownInhibit();
    return SessionBaseWindow::mouseReleaseEvent(event);
}

void WarningContent::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        doCancelShutdownInhibit();
        break;
    }
    QWidget::keyPressEvent(event);
}
