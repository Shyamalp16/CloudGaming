#pragma once

#define CLOUD_GAMING_VERSION_MAJOR 0
#define CLOUD_GAMING_VERSION_MINOR 1
#define CLOUD_GAMING_VERSION_PATCH 0
#define CLOUD_GAMING_VERSION_BUILD 0
#define CLOUD_GAMING_VERSION "0.1.0"
#define CLOUD_GAMING_VERSION_W L"0.1.0"

// Release CI must replace these with administrator-controlled values. They are
// deliberately compiled into the binary so a user-writable config file cannot
// replace the updater's trust root.
#ifndef CLOUD_GAMING_UPDATE_FEED_URL
#define CLOUD_GAMING_UPDATE_FEED_URL ""
#endif
#ifndef CLOUD_GAMING_UPDATE_CERT_SHA256
#define CLOUD_GAMING_UPDATE_CERT_SHA256 ""
#endif
#ifndef CLOUD_GAMING_UPDATE_CERT_SHA256_NEXT
#define CLOUD_GAMING_UPDATE_CERT_SHA256_NEXT ""
#endif
