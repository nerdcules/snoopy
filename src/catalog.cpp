#include "catalog.h"

#include "parsers.h"

#include <QRegularExpression>

#include <algorithm>

namespace catalog {

using Pair = QPair<QString, Info>;

// The tables below are catalog.py's, entry for entry and in its order.

static const QList<Pair> &permissionTable() {
    static const QList<Pair> table = {
        {QStringLiteral("android.permission.ACCESS_FINE_LOCATION"), {QStringLiteral("Precise location"), QStringLiteral("GPS-level location of the device.")}},
        {QStringLiteral("android.permission.ACCESS_COARSE_LOCATION"), {QStringLiteral("Approximate location"), QStringLiteral("Cell and Wi-Fi location of the device.")}},
        {QStringLiteral("android.permission.ACCESS_BACKGROUND_LOCATION"), {QStringLiteral("Background location"), QStringLiteral("Location while the app is not on screen.")}},
        {QStringLiteral("android.permission.READ_CONTACTS"), {QStringLiteral("Contacts"), QStringLiteral("Names, phone numbers, and emails in the address book.")}},
        {QStringLiteral("android.permission.WRITE_CONTACTS"), {QStringLiteral("Contacts"), QStringLiteral("Changes to the address book.")}},
        {QStringLiteral("android.permission.READ_CALL_LOG"), {QStringLiteral("Call history"), QStringLiteral("Numbers called and received, and when.")}},
        {QStringLiteral("android.permission.READ_SMS"), {QStringLiteral("SMS messages"), QStringLiteral("Text message contents.")}},
        {QStringLiteral("android.permission.RECEIVE_SMS"), {QStringLiteral("Incoming SMS"), QStringLiteral("Text messages as they arrive.")}},
        {QStringLiteral("android.permission.SEND_SMS"), {QStringLiteral("SMS sending"), QStringLiteral("The app can send text messages.")}},
        {QStringLiteral("android.permission.READ_PHONE_STATE"), {QStringLiteral("Phone identity"), QStringLiteral("Call state and, on older Android versions, hardware identifiers.")}},
        {QStringLiteral("android.permission.READ_PHONE_NUMBERS"), {QStringLiteral("Phone number"), QStringLiteral("The device phone number.")}},
        {QStringLiteral("android.permission.GET_ACCOUNTS"), {QStringLiteral("Accounts"), QStringLiteral("Account names on the device.")}},
        {QStringLiteral("android.permission.CAMERA"), {QStringLiteral("Camera"), QStringLiteral("Images and video from the camera.")}},
        {QStringLiteral("android.permission.RECORD_AUDIO"), {QStringLiteral("Microphone"), QStringLiteral("Audio from the microphone.")}},
        {QStringLiteral("android.permission.READ_EXTERNAL_STORAGE"), {QStringLiteral("Files"), QStringLiteral("Shared files on the device.")}},
        {QStringLiteral("android.permission.READ_MEDIA_IMAGES"), {QStringLiteral("Photos"), QStringLiteral("Images in shared storage.")}},
        {QStringLiteral("android.permission.READ_MEDIA_VIDEO"), {QStringLiteral("Videos"), QStringLiteral("Videos in shared storage.")}},
        {QStringLiteral("android.permission.READ_MEDIA_AUDIO"), {QStringLiteral("Audio files"), QStringLiteral("Audio files in shared storage.")}},
        {QStringLiteral("android.permission.ACCESS_MEDIA_LOCATION"), {QStringLiteral("Photo locations"), QStringLiteral("Location tags embedded in photos.")}},
        {QStringLiteral("android.permission.BLUETOOTH_SCAN"), {QStringLiteral("Nearby Bluetooth devices"), QStringLiteral("Devices found by a Bluetooth scan.")}},
        {QStringLiteral("android.permission.BLUETOOTH_CONNECT"), {QStringLiteral("Bluetooth connections"), QStringLiteral("Identity of connected Bluetooth devices.")}},
        {QStringLiteral("android.permission.NEARBY_WIFI_DEVICES"), {QStringLiteral("Nearby Wi-Fi devices"), QStringLiteral("Devices seen over nearby Wi-Fi.")}},
        {QStringLiteral("android.permission.BODY_SENSORS"), {QStringLiteral("Body sensors"), QStringLiteral("Heart rate and similar body-sensor readings.")}},
        {QStringLiteral("android.permission.ACTIVITY_RECOGNITION"), {QStringLiteral("Physical activity"), QStringLiteral("Walking, driving, and similar motion.")}},
        {QStringLiteral("android.permission.READ_CALENDAR"), {QStringLiteral("Calendar"), QStringLiteral("Calendar events.")}},
        {QStringLiteral("android.permission.PACKAGE_USAGE_STATS"), {QStringLiteral("App usage"), QStringLiteral("Which apps are used and when.")}},
        {QStringLiteral("com.google.android.gms.permission.AD_ID"), {QStringLiteral("Advertising ID"), QStringLiteral("The resettable advertising identifier.")}},
        {QStringLiteral("android.permission.QUERY_ALL_PACKAGES"), {QStringLiteral("Installed apps"), QStringLiteral("The list of apps installed on the device.")}},
        {QStringLiteral("android.permission.SYSTEM_ALERT_WINDOW"), {QStringLiteral("Screen overlay"), QStringLiteral("Windows drawn on top of other apps.")}},
        {QStringLiteral("android.permission.BIND_ACCESSIBILITY_SERVICE"), {QStringLiteral("Accessibility"), QStringLiteral("Screen content and input when the accessibility service is on.")}},
    };
    return table;
}

static const QList<Pair> &opTable() {
    static const QList<Pair> table = {
        {QStringLiteral("FINE_LOCATION"), {QStringLiteral("Precise location"), QStringLiteral("GPS-level location of the device.")}},
        {QStringLiteral("COARSE_LOCATION"), {QStringLiteral("Approximate location"), QStringLiteral("Cell and Wi-Fi location of the device.")}},
        {QStringLiteral("GPS"), {QStringLiteral("GPS"), QStringLiteral("Satellite location fixes.")}},
        {QStringLiteral("MONITOR_LOCATION"), {QStringLiteral("Continuous location"), QStringLiteral("Ongoing location monitoring.")}},
        {QStringLiteral("MONITOR_HIGH_POWER_LOCATION"), {QStringLiteral("High-power location"), QStringLiteral("High-accuracy location that keeps extra hardware awake.")}},
        {QStringLiteral("READ_CONTACTS"), {QStringLiteral("Contacts"), QStringLiteral("Names, phone numbers, and emails in the address book.")}},
        {QStringLiteral("WRITE_CONTACTS"), {QStringLiteral("Contacts"), QStringLiteral("Changes to the address book.")}},
        {QStringLiteral("READ_CALL_LOG"), {QStringLiteral("Call history"), QStringLiteral("Numbers called and received, and when.")}},
        {QStringLiteral("WRITE_CALL_LOG"), {QStringLiteral("Call history"), QStringLiteral("Changes to call history.")}},
        {QStringLiteral("READ_CALENDAR"), {QStringLiteral("Calendar"), QStringLiteral("Calendar events.")}},
        {QStringLiteral("WIFI_SCAN"), {QStringLiteral("Wi-Fi scan"), QStringLiteral("Nearby network names and signal strength, which can locate the device.")}},
        {QStringLiteral("READ_SMS"), {QStringLiteral("SMS messages"), QStringLiteral("Text message contents.")}},
        {QStringLiteral("RECEIVE_SMS"), {QStringLiteral("Incoming SMS"), QStringLiteral("Text messages as they arrive.")}},
        {QStringLiteral("SEND_SMS"), {QStringLiteral("SMS sending"), QStringLiteral("Text messages the app sends.")}},
        {QStringLiteral("CAMERA"), {QStringLiteral("Camera"), QStringLiteral("Images and video from the camera.")}},
        {QStringLiteral("RECORD_AUDIO"), {QStringLiteral("Microphone"), QStringLiteral("Audio from the microphone.")}},
        {QStringLiteral("RECORD_AUDIO_HOTWORD"), {QStringLiteral("Microphone hotword"), QStringLiteral("Short microphone captures for a wake word.")}},
        {QStringLiteral("RECORD_AUDIO_OUTPUT"), {QStringLiteral("Playback audio"), QStringLiteral("Audio being played on the device.")}},
        {QStringLiteral("READ_CLIPBOARD"), {QStringLiteral("Clipboard"), QStringLiteral("Text and images currently copied on the device.")}},
        {QStringLiteral("WRITE_CLIPBOARD"), {QStringLiteral("Clipboard"), QStringLiteral("Content the app placed on the clipboard.")}},
        {QStringLiteral("READ_PHONE_STATE"), {QStringLiteral("Phone identity"), QStringLiteral("Call state and, on older Android versions, hardware identifiers.")}},
        {QStringLiteral("READ_PHONE_NUMBERS"), {QStringLiteral("Phone number"), QStringLiteral("The device phone number.")}},
        {QStringLiteral("GET_ACCOUNTS"), {QStringLiteral("Accounts"), QStringLiteral("Account names on the device.")}},
        {QStringLiteral("READ_EXTERNAL_STORAGE"), {QStringLiteral("Files"), QStringLiteral("Shared files on the device.")}},
        {QStringLiteral("READ_MEDIA_IMAGES"), {QStringLiteral("Photos"), QStringLiteral("Images in shared storage.")}},
        {QStringLiteral("READ_MEDIA_VIDEO"), {QStringLiteral("Videos"), QStringLiteral("Videos in shared storage.")}},
        {QStringLiteral("READ_MEDIA_AUDIO"), {QStringLiteral("Audio files"), QStringLiteral("Audio files in shared storage.")}},
        {QStringLiteral("ACCESS_MEDIA_LOCATION"), {QStringLiteral("Photo locations"), QStringLiteral("Location tags embedded in photos.")}},
        {QStringLiteral("BODY_SENSORS"), {QStringLiteral("Body sensors"), QStringLiteral("Heart rate and similar body-sensor readings.")}},
        {QStringLiteral("ACTIVITY_RECOGNITION"), {QStringLiteral("Physical activity"), QStringLiteral("Walking, driving, and similar motion.")}},
        {QStringLiteral("GET_USAGE_STATS"), {QStringLiteral("App usage"), QStringLiteral("Which apps are used and when.")}},
        {QStringLiteral("READ_DEVICE_IDENTIFIERS"), {QStringLiteral("Hardware identifiers"), QStringLiteral("Non-resettable identifiers such as an IMEI or serial number.")}},
        {QStringLiteral("BLUETOOTH_SCAN"), {QStringLiteral("Nearby Bluetooth devices"), QStringLiteral("Devices found by a Bluetooth scan.")}},
        {QStringLiteral("BLUETOOTH_CONNECT"), {QStringLiteral("Bluetooth connections"), QStringLiteral("Identity of connected Bluetooth devices.")}},
        {QStringLiteral("NEARBY_WIFI_DEVICES"), {QStringLiteral("Nearby Wi-Fi devices"), QStringLiteral("Devices seen over nearby Wi-Fi.")}},
        {QStringLiteral("SYSTEM_ALERT_WINDOW"), {QStringLiteral("Screen overlay"), QStringLiteral("Windows drawn on top of other apps.")}},
        {QStringLiteral("BIND_ACCESSIBILITY_SERVICE"), {QStringLiteral("Accessibility"), QStringLiteral("Screen content and input when the accessibility service is on.")}},
        {QStringLiteral("ACCESS_ADSERVICES_AD_ID"), {QStringLiteral("Advertising ID"), QStringLiteral("The resettable advertising identifier.")}},
    };
    return table;
}

const QStringList &categoryOrder() {
    static const QStringList order = {
        QStringLiteral("Precise location"),
        QStringLiteral("Approximate location"),
        QStringLiteral("Background location"),
        QStringLiteral("GPS"),
        QStringLiteral("Continuous location"),
        QStringLiteral("High-power location"),
        QStringLiteral("Camera"),
        QStringLiteral("Microphone"),
        QStringLiteral("Microphone hotword"),
        QStringLiteral("Playback audio"),
        QStringLiteral("Contacts"),
        QStringLiteral("Call history"),
        QStringLiteral("SMS messages"),
        QStringLiteral("Incoming SMS"),
        QStringLiteral("SMS sending"),
        QStringLiteral("Clipboard"),
        QStringLiteral("Phone identity"),
        QStringLiteral("Phone number"),
        QStringLiteral("Hardware identifiers"),
        QStringLiteral("Advertising ID"),
        QStringLiteral("Accounts"),
        QStringLiteral("Calendar"),
        QStringLiteral("Photos"),
        QStringLiteral("Videos"),
        QStringLiteral("Audio files"),
        QStringLiteral("Photo locations"),
        QStringLiteral("Files"),
        QStringLiteral("Body sensors"),
        QStringLiteral("Physical activity"),
        QStringLiteral("App usage"),
        QStringLiteral("Installed apps"),
        QStringLiteral("Nearby Bluetooth devices"),
        QStringLiteral("Bluetooth connections"),
        QStringLiteral("Nearby Wi-Fi devices"),
        QStringLiteral("Wi-Fi scan"),
        QStringLiteral("Screen overlay"),
        QStringLiteral("Accessibility"),
    };
    return order;
}

static const QHash<QString, QString> &modePhrases() {
    static const QHash<QString, QString> phrases = {
        {QStringLiteral("allow"), QStringLiteral("allowed")},
        {QStringLiteral("foreground"), QStringLiteral("allowed only while the app is in use")},
        {QStringLiteral("ignore"), QStringLiteral("denied")},
        {QStringLiteral("deny"), QStringLiteral("denied")},
        {QStringLiteral("default"), QStringLiteral("left at the default mode")},
        {QStringLiteral("errored"), QStringLiteral("errored")},
        {QStringLiteral("ask"), QStringLiteral("requires a prompt")},
    };
    return phrases;
}

const QList<Tracker> &trackers() {
    static const QList<Tracker> list = {
        {QStringLiteral("Firebase Analytics"), QStringLiteral("com.google.firebase.analytics"), QStringLiteral("Usage events, device model, and the advertising ID when the app shares it.")},
        {QStringLiteral("Firebase Crashlytics"), QStringLiteral("com.google.firebase.crashlytics"), QStringLiteral("Crash traces, device model, OS version, and app state at the crash.")},
        {QStringLiteral("Firebase Installations"), QStringLiteral("com.google.firebase.installations"), QStringLiteral("An app-scoped installation identifier.")},
        {QStringLiteral("Firebase Cloud Messaging"), QStringLiteral("com.google.firebase.messaging"), QStringLiteral("A push token. Delivery metadata goes to Google.")},
        {QStringLiteral("Google Mobile Ads"), QStringLiteral("com.google.android.gms.ads"), QStringLiteral("Advertising ID, ad interactions, and IP address.")},
        {QStringLiteral("Google Measurement"), QStringLiteral("com.google.android.gms.measurement"), QStringLiteral("Analytics events and device properties.")},
        {QStringLiteral("Facebook App Events"), QStringLiteral("com.facebook.appevents"), QStringLiteral("App events, device info, and advertising ID.")},
        {QStringLiteral("Facebook Audience Network"), QStringLiteral("com.facebook.ads"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("Facebook Login"), QStringLiteral("com.facebook.FacebookActivity"), QStringLiteral("Facebook identity when the user signs in, plus app events.")},
        {QStringLiteral("AppsFlyer"), QStringLiteral("com.appsflyer"), QStringLiteral("Install attribution, advertising ID, and IP address.")},
        {QStringLiteral("Adjust"), QStringLiteral("com.adjust.sdk"), QStringLiteral("Install attribution, advertising ID, and IP address.")},
        {QStringLiteral("Amplitude"), QStringLiteral("com.amplitude"), QStringLiteral("Product analytics events and device properties.")},
        {QStringLiteral("Mixpanel"), QStringLiteral("com.mixpanel"), QStringLiteral("Product analytics events and device properties.")},
        {QStringLiteral("Branch"), QStringLiteral("io.branch"), QStringLiteral("Attribution identifiers and which link was opened.")},
        {QStringLiteral("Sentry"), QStringLiteral("io.sentry"), QStringLiteral("Crash traces and device state.")},
        {QStringLiteral("Bugsnag"), QStringLiteral("com.bugsnag"), QStringLiteral("Crash traces and device state.")},
        {QStringLiteral("OneSignal"), QStringLiteral("com.onesignal"), QStringLiteral("Push token and device properties.")},
        {QStringLiteral("Braze"), QStringLiteral("com.braze"), QStringLiteral("Engagement events and device properties.")},
        {QStringLiteral("Unity Ads"), QStringLiteral("com.unity3d.ads"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("AppLovin"), QStringLiteral("com.applovin"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("IronSource"), QStringLiteral("com.ironsource"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("Vungle"), QStringLiteral("com.vungle"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("InMobi"), QStringLiteral("com.inmobi"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("Chartboost"), QStringLiteral("com.chartboost"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("Start.io"), QStringLiteral("com.startapp"), QStringLiteral("Advertising ID and, on older SDKs, installed-app signals.")},
        {QStringLiteral("Flurry"), QStringLiteral("com.flurry"), QStringLiteral("Analytics events and device properties.")},
        {QStringLiteral("New Relic"), QStringLiteral("com.newrelic.agent"), QStringLiteral("Performance traces and device state.")},
        {QStringLiteral("AppMetrica"), QStringLiteral("io.appmetrica"), QStringLiteral("Analytics events, device identifiers, and location if the app can read it.")},
        {QStringLiteral("Yandex Metrica"), QStringLiteral("com.yandex.metrica"), QStringLiteral("Analytics events and device identifiers.")},
        {QStringLiteral("Umeng"), QStringLiteral("com.umeng"), QStringLiteral("Analytics events and device identifiers.")},
        {QStringLiteral("Huawei Ads"), QStringLiteral("com.huawei.hms.ads"), QStringLiteral("Advertising ID and ad interactions.")},
        {QStringLiteral("CleverTap"), QStringLiteral("com.clevertap"), QStringLiteral("Engagement events and device properties.")},
        {QStringLiteral("MoEngage"), QStringLiteral("com.moengage"), QStringLiteral("Engagement events and device properties.")},
        {QStringLiteral("Kochava"), QStringLiteral("com.kochava"), QStringLiteral("Attribution identifiers and IP address.")},
        {QStringLiteral("Singular"), QStringLiteral("com.singular"), QStringLiteral("Attribution identifiers and IP address.")},
        {QStringLiteral("mParticle"), QStringLiteral("com.mparticle"), QStringLiteral("Event routing, device properties, and advertising ID.")},
        {QStringLiteral("Segment"), QStringLiteral("com.segment.analytics"), QStringLiteral("Event routing and device properties.")},
        {QStringLiteral("Adobe Marketing"), QStringLiteral("com.adobe.marketing"), QStringLiteral("Engagement events and device properties.")},
        {QStringLiteral("Salesforce Marketing Cloud"), QStringLiteral("com.salesforce.marketingcloud"), QStringLiteral("Engagement events and contact keys the app sets.")},
        {QStringLiteral("Amazon Ads"), QStringLiteral("com.amazon.device.ads"), QStringLiteral("Advertising ID and ad interactions.")},
    };
    return list;
}

const QList<QPair<QString, QString>> &trackerDomains() {
    static const QList<QPair<QString, QString>> list = {
        {QStringLiteral("app-measurement.com"), QStringLiteral("Firebase Analytics")},
        {QStringLiteral("app-analytics-services.com"), QStringLiteral("Google Analytics")},
        {QStringLiteral("google-analytics.com"), QStringLiteral("Google Analytics")},
        {QStringLiteral("firebaselogging.googleapis.com"), QStringLiteral("Firebase Crashlytics")},
        {QStringLiteral("firebaseinstallations.googleapis.com"), QStringLiteral("Firebase Installations")},
        {QStringLiteral("crashlytics.com"), QStringLiteral("Firebase Crashlytics")},
        {QStringLiteral("doubleclick.net"), QStringLiteral("Google Ads")},
        {QStringLiteral("googlesyndication.com"), QStringLiteral("Google Ads")},
        {QStringLiteral("googleadservices.com"), QStringLiteral("Google Ads")},
        {QStringLiteral("graph.facebook.com"), QStringLiteral("Facebook")},
        {QStringLiteral("graph.facebook.net"), QStringLiteral("Facebook")},
        {QStringLiteral("appsflyer.com"), QStringLiteral("AppsFlyer")},
        {QStringLiteral("onelink.me"), QStringLiteral("AppsFlyer")},
        {QStringLiteral("adjust.com"), QStringLiteral("Adjust")},
        {QStringLiteral("adjust.io"), QStringLiteral("Adjust")},
        {QStringLiteral("amplitude.com"), QStringLiteral("Amplitude")},
        {QStringLiteral("mixpanel.com"), QStringLiteral("Mixpanel")},
        {QStringLiteral("branch.io"), QStringLiteral("Branch")},
        {QStringLiteral("app.link"), QStringLiteral("Branch")},
        {QStringLiteral("sentry.io"), QStringLiteral("Sentry")},
        {QStringLiteral("bugsnag.com"), QStringLiteral("Bugsnag")},
        {QStringLiteral("onesignal.com"), QStringLiteral("OneSignal")},
        {QStringLiteral("braze.com"), QStringLiteral("Braze")},
        {QStringLiteral("appboy.com"), QStringLiteral("Braze")},
        {QStringLiteral("applovin.com"), QStringLiteral("AppLovin")},
        {QStringLiteral("unityads.unity3d.com"), QStringLiteral("Unity Ads")},
        {QStringLiteral("supersonicads.com"), QStringLiteral("IronSource")},
        {QStringLiteral("vungle.com"), QStringLiteral("Vungle")},
        {QStringLiteral("inmobi.com"), QStringLiteral("InMobi")},
        {QStringLiteral("chartboost.com"), QStringLiteral("Chartboost")},
        {QStringLiteral("startappservice.com"), QStringLiteral("Start.io")},
        {QStringLiteral("flurry.com"), QStringLiteral("Flurry")},
        {QStringLiteral("appmetrica.yandex.net"), QStringLiteral("AppMetrica")},
        {QStringLiteral("appmetrica.yandex.com"), QStringLiteral("AppMetrica")},
        {QStringLiteral("umeng.com"), QStringLiteral("Umeng")},
        {QStringLiteral("clevertap-prod.com"), QStringLiteral("CleverTap")},
        {QStringLiteral("moengage.com"), QStringLiteral("MoEngage")},
        {QStringLiteral("kochava.com"), QStringLiteral("Kochava")},
        {QStringLiteral("singular.net"), QStringLiteral("Singular")},
        {QStringLiteral("mparticle.com"), QStringLiteral("mParticle")},
        {QStringLiteral("segment.io"), QStringLiteral("Segment")},
        {QStringLiteral("segment.com"), QStringLiteral("Segment")},
        {QStringLiteral("demdex.net"), QStringLiteral("Adobe Audience Manager")},
        {QStringLiteral("omtrdc.net"), QStringLiteral("Adobe Analytics")},
        {QStringLiteral("newrelic.com"), QStringLiteral("New Relic")},
        {QStringLiteral("nr-data.net"), QStringLiteral("New Relic")},
    };
    return list;
}

const QStringList &sources() {
    static const QStringList list = {
        QStringLiteral("dumpsys appops"),
        QStringLiteral("dumpsys package"),
        QStringLiteral("dumpsys activity"),
        QStringLiteral("dumpsys jobscheduler"),
        QStringLiteral("dumpsys power"),
        QStringLiteral("dumpsys location"),
        QStringLiteral("dumpsys sensorservice"),
        QStringLiteral("dumpsys clipboard"),
        QStringLiteral("dumpsys netstats"),
        QStringLiteral("/proc/net/tcp"),
        QStringLiteral("/proc/net/udp"),
        QStringLiteral("logcat DNS"),
    };
    return list;
}

const QStringList &limits() {
    static const QStringList list = {
        QStringLiteral("Network sockets belong to a Linux uid. On Android that uid is usually one app."),
        QStringLiteral("Transmission shown here is destination, size, and DNS name. HTTPS message bodies stay encrypted."),
        QStringLiteral("Tracker SDK rows are components shipped inside the app and the kind of data that SDK is built to collect."),
    };
    return list;
}

static QHash<QString, Info> indexed(const QList<Pair> &table) {
    QHash<QString, Info> index;
    for (const Pair &pair : table)
        index.insert(pair.first, pair.second);
    return index;
}

const Info *permissionInfo(const QString &permission) {
    static const QHash<QString, Info> index = indexed(permissionTable());
    auto found = index.constFind(permission);
    return found == index.cend() ? nullptr : &*found;
}

const Info *opInfo(const QString &op) {
    static const QHash<QString, Info> index = indexed(opTable());
    auto found = index.constFind(op);
    return found == index.cend() ? nullptr : &*found;
}

int categoryRank(const QString &category) {
    const qsizetype index = categoryOrder().indexOf(category);
    return int(index < 0 ? categoryOrder().size() : index);
}

QString modePhrase(const std::optional<QString> &mode) {
    if (!mode || mode->isEmpty())
        return QStringLiteral("mode unrecorded");
    return modePhrases().value(*mode, *mode);
}

std::optional<QString> trackerForHost(const QString &host) {
    QString name = host.toLower();
    while (name.endsWith(u'.'))
        name.chop(1);
    std::optional<QString> best;
    qsizetype bestLen = -1;
    for (const auto &[rawDomain, label] : trackerDomains()) {
        const QString domain = rawDomain.toLower();
        if (name == domain || name.endsWith(u'.' + domain)) {
            if (domain.size() > bestLen) {
                best = label;
                bestLen = domain.size();
            }
        }
    }
    return best;
}

QList<Tracker> findTrackers(const QString &text) {
    struct Entry {
        Tracker tracker;
        QRegularExpression pattern;
    };
    static const QList<Entry> byLength = [] {
        QList<Tracker> sorted = trackers();
        // sorted(..., key=len(prefix), reverse=True) keeps equal lengths in order.
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const Tracker &a, const Tracker &b) { return a.prefix.size() > b.prefix.size(); });
        QList<Entry> entries;
        for (const Tracker &tracker : sorted)
            entries.append({tracker, QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_])") +
                                                        QRegularExpression::escape(tracker.prefix) +
                                                        QStringLiteral("(?![A-Za-z0-9_])"))});
        return entries;
    }();
    QList<Tracker> found;
    for (const Entry &entry : byLength) {
        // The pattern needs the literal prefix; a plain search rules most out.
        if (!text.contains(entry.tracker.prefix) || !entry.pattern.match(text).hasMatch())
            continue;
        const bool covered = std::any_of(found.cbegin(), found.cend(), [&](const Tracker &existing) {
            return existing.prefix.startsWith(entry.tracker.prefix);
        });
        if (covered)
            continue;
        found.append(entry.tracker);
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Tracker &a, const Tracker &b) { return py::less(a.name, b.name); });
    return found;
}

} // namespace catalog
