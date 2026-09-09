#pragma once

/*
 * Pulsar keeps the stock DirectShow names only for a process that has no
 * runtime namespace configuration at all.  An explicit configuration that
 * cannot name a dedicated mapping must fail closed instead of falling back
 * into another process's legacy mapping.
 *
 * This header is deliberately C-compatible: the producer is C and the
 * DirectShow consumer is C++.  It is private to win-dshow and does not add a
 * libobs dependency to the virtual-camera DLL.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

enum directshow_queue_namespace {
	DIRECTSHOW_QUEUE_NAMESPACE_REJECT = 0,
	DIRECTSHOW_QUEUE_NAMESPACE_LEGACY,
	DIRECTSHOW_QUEUE_NAMESPACE_DEDICATED,
};

enum directshow_consumer_filter_kind {
	DIRECTSHOW_CONSUMER_FILTER_STOCK = 0,
	DIRECTSHOW_CONSUMER_FILTER_PROGRAM_RETURN,
	DIRECTSHOW_CONSUMER_FILTER_PREVIEW_RETURN,
};

static inline bool directshow_environment_variable(const char *name, char *value, size_t capacity)
{
	/* getenv() alone cannot distinguish an unset Windows variable from one
	 * explicitly set to empty, which is security-significant here. */
	SetLastError(ERROR_SUCCESS);
	DWORD size = GetEnvironmentVariableA(name, value, (DWORD)capacity);
	if (size == 0) {
		if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
			return false;
		value[0] = '\0';
		return true;
	}
	if (size >= capacity) {
		/* Environment values at the Windows limit cannot be a valid runtime
		 * ID or alias.  Leave an explicitly invalid marker for the decision. */
		value[0] = '!';
		value[1] = '\0';
	}
	return true;
}

static inline bool directshow_command_line_switch(const char *name, char *value, size_t capacity)
{
	const char *command_line = GetCommandLineA();
	if (!command_line || !*command_line || !name || !*name || capacity < 2)
		return false;

	char prefix[128] = {0};
	const int prefix_length = _snprintf_s(prefix, sizeof(prefix), _TRUNCATE, "--%s=", name);
	if (prefix_length <= 0)
		return false;

	for (const char *cursor = command_line; *cursor; ++cursor) {
		const char previous = cursor == command_line ? ' ' : cursor[-1];
		if (previous != ' ' && previous != '\t' && previous != '"')
			continue;
		if (strncmp(cursor, prefix, (size_t)prefix_length) != 0)
			continue;

		const char *start = cursor + prefix_length;
		const char *end = start;
		while (*end && *end != ' ' && *end != '\t' && *end != '"')
			++end;
		const size_t length = (size_t)(end - start);
		if (length == 0 || length >= capacity) {
			/* A present but malformed/oversized switch is explicit invalid
			 * configuration, not permission to fall back to a legacy alias. */
			value[0] = '!';
			value[1] = '\0';
		} else {
			memcpy(value, start, length);
			value[length] = '\0';
		}
		return true;
	}

	return false;
}

static inline bool directshow_runtime_instance_id_value(char *value, size_t capacity)
{
	/* An explicit Chromium switch is the host's authoritative value.  This
	 * protects the consumer from an inherited stale environment block while
	 * preserving the environment path for the Pulsar producer process. */
	if (directshow_command_line_switch("pulsar-runtime-instance-id", value, capacity))
		return true;
	return directshow_environment_variable("PULSAR_RUNTIME_INSTANCE_ID", value, capacity);
}

static inline bool directshow_legacy_alias_value(char *value, size_t capacity)
{
	if (directshow_command_line_switch("pulsar-directshow-legacy-alias", value, capacity))
		return true;
	return directshow_environment_variable("PULSAR_DIRECTSHOW_LEGACY_ALIAS", value, capacity);
}

static inline bool directshow_runtime_instance_id_valid(const char *value)
{
	if (!value || !*value || strlen(value) > 64)
		return false;
	if (!((*value >= 'A' && *value <= 'Z') || (*value >= 'a' && *value <= 'z') ||
	      (*value >= '0' && *value <= '9')))
		return false;
	for (const char *p = value; *p; ++p) {
		if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
		      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
			return false;
	}
	return strcmp(value, ".") != 0 && strcmp(value, "..") != 0;
}

static inline bool directshow_legacy_alias_truthy(const char *value)
{
	return value && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
			 strcmp(value, "yes") == 0 || strcmp(value, "on") == 0);
}

static inline enum directshow_queue_namespace directshow_queue_namespace_for_consumer(
	enum directshow_consumer_filter_kind filter_kind)
{
	char runtime_id[32768] = {0};
	char legacy_alias[32768] = {0};
	const bool runtime_id_present =
		directshow_runtime_instance_id_value(runtime_id, sizeof(runtime_id));
	const bool legacy_alias_present =
		directshow_legacy_alias_value(legacy_alias, sizeof(legacy_alias));

	if (runtime_id_present && !directshow_runtime_instance_id_valid(runtime_id))
		return DIRECTSHOW_QUEUE_NAMESPACE_REJECT;
	if (runtime_id_present)
		return legacy_alias_present && directshow_legacy_alias_truthy(legacy_alias)
			       ? DIRECTSHOW_QUEUE_NAMESPACE_LEGACY
			       : DIRECTSHOW_QUEUE_NAMESPACE_DEDICATED;

	if (legacy_alias_present)
		return DIRECTSHOW_QUEUE_NAMESPACE_REJECT;

	/* Only the stock OBS filter preserves the historical implicit alias.  The
	 * Pulsar Program/Preview CLSIDs must be bound to an explicit runtime. */
	return filter_kind == DIRECTSHOW_CONSUMER_FILTER_STOCK
		       ? DIRECTSHOW_QUEUE_NAMESPACE_LEGACY
		       : DIRECTSHOW_QUEUE_NAMESPACE_REJECT;
}

static inline enum directshow_queue_namespace directshow_queue_namespace_from_environment(void)
{
	return directshow_queue_namespace_for_consumer(DIRECTSHOW_CONSUMER_FILTER_STOCK);
}

static inline const char *directshow_queue_namespace_name(enum directshow_queue_namespace name)
{
	switch (name) {
	case DIRECTSHOW_QUEUE_NAMESPACE_LEGACY:
		return "legacy";
	case DIRECTSHOW_QUEUE_NAMESPACE_DEDICATED:
		return "dedicated";
	default:
		return "reject";
	}
}
