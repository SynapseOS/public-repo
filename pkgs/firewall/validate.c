// validate.c: проверка пользовательской конфигурации фильтра
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: пользовательский firewall
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный
#include <stdbool.h>
#include <stddef.h>
#include <dev/firewall.h>

// Проверить опубликованные поля ABI до отправки конфигурации ядру
bool syn_fw_validate(const struct syn_fw_config *config) {
	if (config == NULL || config->version != SYN_FW_VERSION || config->enabled > 1 ||
	    config->count > SYN_FW_MAX_RULES || config->reserved[0] != 0 || config->reserved[1] != 0) {
		return false;
	}

	for (unsigned direction = 0; direction < 2; direction++) {
		if (config->policy[direction] > SYN_FW_ALLOW) {
			return false;
		}
	}

	for (unsigned index = 0; index < config->count; index++) {
		const struct syn_fw_rule *rule = &config->rules[index];
		unsigned address_bits = 0;

		if (rule->family == 4) {
			address_bits = 32;
		} else if (rule->family == 6) {
			address_bits = 128;
		} else if (rule->family != 0) {
			return false;
		}

		if (rule->direction > SYN_FW_OUT || rule->action > SYN_FW_ALLOW ||
		    rule->src_prefix > address_bits || rule->dst_prefix > address_bits ||
		    rule->sport_lo > rule->sport_hi || rule->dport_lo > rule->dport_hi ||
		    rule->reserved[0] != 0 || rule->reserved[1] != 0) {
			return false;
		}

		switch (rule->protocol) {
			case 0:
			case 1:
			case 6:
			case 17:
			case 58:
				break;
			default:
				return false;
		}

		if ((rule->protocol == 1 && rule->family == 6) ||
		    (rule->protocol == 58 && rule->family == 4)) {
			return false;
		}

		if (rule->protocol != 6 && rule->protocol != 17 &&
		    (rule->sport_lo != 0 || rule->sport_hi != 65535 || rule->dport_lo != 0 ||
		     rule->dport_hi != 65535)) {
			return false;
		}
	}

	return true;
}
