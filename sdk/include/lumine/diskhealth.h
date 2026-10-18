/*
    diskhealth.h: Структура сведений о состоянии диска
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Структура сведений о состоянии диска, используемые ядром и приложениями
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: 13.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef LUMINE_DISKHEALTH_H
#define LUMINE_DISKHEALTH_H

#include <stdint.h>

// Запрос только читает состояние диска, произвольные команды не передаются
#define DISK_HEALTH_GET 0x53444801u
#define DISK_HEALTH_VERSION 1u
#define DISK_HEALTH_ATA 1u
#define DISK_HEALTH_NVME 2u
#define DISK_HEALTH_IDENTITY (1u << 0)
#define DISK_HEALTH_SUPPORTED (1u << 1)
#define DISK_HEALTH_ENABLED (1u << 2)
#define DISK_HEALTH_STATUS_VALID (1u << 3)
#define DISK_HEALTH_FAILING (1u << 4)
#define DISK_HEALTH_DATA_VALID (1u << 5)
#define DISK_HEALTH_THRESHOLDS_VALID (1u << 6)

struct disk_health {
	uint32_t version;
	uint32_t protocol;
	uint32_t flags;
	uint32_t error;
	uint8_t identity[512]; // ATA IDENTIFY либо первые 512 байт NVMe Identify Controller
	uint8_t data[512]; // ATA SMART READ DATA либо общий журнал NVMe Health 02h
	uint8_t thresholds[512]; // ATA SMART READ THRESHOLDS, если поддерживается
};

#endif
