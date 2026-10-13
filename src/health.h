#pragma once

#include "common.h"

namespace mtm {

enum class Health { Unknown, Good, Warning, Bad };

struct DriveAttribute {
    std::wstring name, value, detail;
    Health health = Health::Unknown;
};

struct DriveHealth {
    int number = 0;                       // \\.\PhysicalDriveN

    std::wstring model, serial, firmware, bus, kind;

    uint64_t size = 0;                    // bytes

    int temperatureC = -1;                // °C, -1 = unknown

    // NVMe-only fields. Both use sentinel values so the UI can hide them
    // for drives where they don't apply.
    int      powerOnHours = -1;           // -1 = unknown (populated for NVMe and ATA id 9)
    uint64_t bytesWritten = 0;            // 0  = unknown (NVMe only)
    int      percentUsed  = -1;           // -1 = unknown (NVMe wear estimate)

    Health health = Health::Unknown;
    std::wstring summary;                 // one-line verdict

    std::vector<DriveAttribute> attributes;
};

// Reads identity, temperature, and the NVMe health log or ATA SMART table
// for every physical drive.
//
// Requires administrator rights for health data on most systems; identity
// (model, serial, firmware, bus, size) is available without elevation.
// Never throws; drives that can't be opened or that don't expose health
// data are returned with health == Health::Unknown and a summary string
// explaining why.
std::vector<DriveHealth> QueryDriveHealth();

const wchar_t* HealthName(Health h);

}  // namespace mtm