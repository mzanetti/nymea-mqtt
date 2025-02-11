/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*
*  Copyright 2013 - 2022, nymea GmbH
*  Contact: contact@nymea.io
*
*  This file is part of nymea.
*  This project including source code and documentation is protected by copyright law, and
*  remains the property of nymea GmbH. All rights, including reproduction, publication,
*  editing and translation, are reserved. The use of this project is subject to the terms of a
*  license agreement to be concluded with nymea GmbH in accordance with the terms
*  of use of nymea GmbH, available under https://nymea.io/license
*
*  GNU General Public License Usage
*  Alternatively, this project may be redistributed and/or modified under
*  the terms of the GNU General Public License as published by the Free Software Foundation,
*  GNU version 3. this project is distributed in the hope that it will be useful, but WITHOUT ANY
*  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
*  PURPOSE. See the GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License along with this project.
*  If not, see <https://www.gnu.org/licenses/>.
*
*  For any further details and any questions please contact us under contact@nymea.io
*  or see our FAQ/Licensing Information on https://nymea.io/license/faq
*
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "mqttclient.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QUuid>
#include <QHostAddress>


int main(int argc, char *argv[]) {

    QCoreApplication *app = new QCoreApplication(argc, argv);

    QCommandLineParser parser;
    parser.addPositionalArgument("server", "The server address.");
    parser.addOptions({
          {{"clientid", "c"}, "The client ID to use for the connection (default: autogenerated).", "clientId", QUuid::createUuid().toString()},
          {{"username", "u"}, "The user name to use for the connection.", "username", ""},
          {{"password", "P"}, "The password to use for the connection.", "password", ""},
          {{"subscribe", "s"}, "Subscribe to a topic filter.", "topicfilter"},
          {{"publish", "p"}, "Publish to topic.", "topic"},
          {{"payload", "l"}, "Publish payload (requires -p).", "payload"},
          {{"retain", "r"}, "Retain flag for publishes (default: no retaining)."},
          {{"qos", "q"}, "QoS (default: 1).", "QoS", "1"},
          {{"ssl", "S"}, "Use SSL for TCP connections (default: off) (Websocket connections will determine the use of SSL using the url scheme)."},
          {{"accept-self-signed-certificate", "A"}, "Ignore self signed certificate errors"},
          {{"verbose", "v"}, "Be more verbose"}
      });
    parser.addHelpOption();

    parser.process(app->arguments());

    if (parser.positionalArguments().count() < 1) {
        parser.showHelp(-1);
    }

    QString verbose = parser.isSet("verbose") ? "true" : "false";
    QLoggingCategory::setFilterRules(QString("nymea.mqtt.client.debug=%1\nnymea.mqtt.client.warning=%2\n").arg(verbose).arg(verbose));

    bool ok;
    int qosInt = parser.value("qos").toInt(&ok);
    if (!ok || qosInt < 0 || qosInt > 2) {
        qCritical() << "Invalid QoS option. Options are 0, 1 or 2.";
        exit(EXIT_FAILURE);
    }
    Mqtt::QoS qos = static_cast<Mqtt::QoS>(qosInt);

    bool retain = parser.isSet("retain");

    MqttClient client(parser.value("clientid"), app);
    QObject::connect(&client, &MqttClient::error, app, [&client, app](QAbstractSocket::SocketError socketError){
        qCritical() << "Connection error:" << socketError;
        client.setAutoReconnect(false);
        app->quit();
    });
    QObject::connect(&client, &MqttClient::sslErrors, app, [&client, &parser](const QList<QSslError> &sslErrors){
        QSslCertificate certificate = sslErrors.first().certificate();
        if (parser.isSet("accept-self-signed-certificate")) {
            QList<QSslError> copy = sslErrors;
            copy.removeOne(QSslError(QSslError::HostNameMismatch, certificate));
            copy.removeAll(QSslError(QSslError::SelfSignedCertificate, certificate));
            if (copy.isEmpty()) {
                qInfo() << "Accepting self signed certificate";
                client.ignoreSslErrors();
                return;
            }
        }
        qWarning() << "SSL errors for certificate:" << qUtf8Printable(certificate.toText()) << sslErrors;
    });
    QObject::connect(&client, &MqttClient::connected, app, [&parser, &client, qos, retain](Mqtt::ConnectReturnCode connectReturnCode, Mqtt::ConnackFlags /*connackFlags*/){
        switch (connectReturnCode) {
        case Mqtt::ConnectReturnCodeIdentifierRejected:
            qCritical() << "Connection failed: Identifier rejected";
            exit(EXIT_FAILURE);
        case Mqtt::ConnectReturnCodeUnacceptableProtocolVersion:
            qCritical() << "Connection failed: Unsupported MQTT protocol version";
            exit(EXIT_FAILURE);
        case Mqtt::ConnectReturnCodeBadUsernameOrPassword:
            qCritical() << "Connection failed: Bad username or password";
            exit(EXIT_FAILURE);
        case Mqtt::ConnectReturnCodeNotAuthorized:
            qCritical() << "Connection failed: Not authorized";
            exit(EXIT_FAILURE);
        case Mqtt::ConnectReturnCodeServerUnavailable:
            qCritical() << "Connection failed: Server unavailable";
            exit(EXIT_FAILURE);
        default:
            qInfo() << "Connected to server.";
        }

        foreach (const QString &topicFilter, parser.values("subscribe")) {
            qDebug() << "Subscribing to" << topicFilter;
            client.subscribe(topicFilter, qos);
        }

        for (int i = 0; i < parser.values("publish").length(); i++) {
            QString topic = parser.values("publish").at(0);
            QByteArray payload;
            if (parser.values("payload").length() > i) {
                payload = parser.values("payload").at(i).toUtf8();
            }
            qDebug().nospace() << "Publishing to " << topic << (!payload.isEmpty() ? ": " + payload : "");
            client.publish(parser.value("publish"), parser.value("payload").toUtf8(), qos, retain);
        }
    });
    QObject::connect(&client, &MqttClient::subscribed, app, [](const QString &topic, Mqtt::SubscribeReturnCode subscribeReturnCode){
        if (subscribeReturnCode == Mqtt::SubscribeReturnCodeFailure) {
            qWarning() << "Subscribing to topic" << topic << "failed.";
        } else {
            qInfo() << "Subscribed to topic filter" << topic << "with QoS" << subscribeReturnCode;
        }
    });
    QObject::connect(&client, &MqttClient::published, app, [](quint16 /*packetId*/, const QString &topic){
        qInfo() << "Published to topic" << topic;
    });
    QObject::connect(&client, &MqttClient::publishReceived, app, [](const QString &topic, const QByteArray &payload, bool retained){
        qInfo().nospace() << "Publish received on topic " << topic << ": " << payload << (retained ? " (retained message)" : "");
    });

    QString server = parser.positionalArguments().at(0);
    if (!server.startsWith("ws://") && !server.startsWith("wss://")) {
        server.prepend("mqtt://");
    }

    QUrl serverUrl = QUrl(server);
    if (!serverUrl.isValid() || !QStringList({"mqtt", "ws", "wss"}).contains(serverUrl.scheme())) {
        qCritical() << "Invalid server argument. Examples:";
        qCritical() << "192.168.0.1:1883";
        qCritical() << "example.com:1883";
        qCritical() << "ws://192.168.0.1:80";
        qCritical() << "wss://example.com:443";
        exit(EXIT_FAILURE);
    }

    if (parser.isSet("username")) {
        client.setUsername(parser.value("username"));
    }
    if (parser.isSet("password")) {
        client.setPassword(parser.value("password"));
    }

    if (serverUrl.scheme() == "mqtt") {
        qDebug() << "Connecting to server" << serverUrl.host() << serverUrl.port(1883);
        client.connectToHost(serverUrl.host(), serverUrl.port(1883), true, parser.isSet("ssl"));
    } else {
        qDebug() << "Connecting to web socket server" << serverUrl.toString();
        QNetworkRequest request(serverUrl);
        client.connectToHost(request);
    }

    return app->exec();
}
