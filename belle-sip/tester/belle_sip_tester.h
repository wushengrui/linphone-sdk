/*
 * Copyright (c) 2012-2019 Belledonne Communications SARL.
 *
 * This file is part of belle-sip.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _BELLE_SIP_TESTER_H
#define _BELLE_SIP_TESTER_H

#include "bctoolbox/tester.h"

#include "belle-sip/belle-sip.h"

#define MULTIPART_BOUNDARY "---------------------------14737809831466499882746641449"

#ifdef __cplusplus
extern "C" {
#endif

extern const char *belle_sip_userhostsfile;
extern const char *belle_sip_tester_root_ca;
extern const char *belle_sip_test_domain;
extern const char *belle_sip_auth_domain;

extern test_suite_t cast_test_suite;
extern test_suite_t generic_uri_test_suite;
extern test_suite_t sip_uri_test_suite;
extern test_suite_t fast_sip_uri_test_suite;
extern test_suite_t perf_sip_uri_test_suite;
extern test_suite_t headers_test_suite;
extern test_suite_t core_test_suite;
extern test_suite_t sdp_test_suite;
extern test_suite_t sdp_belr_test_suite;
extern test_suite_t resolver_test_suite;
extern test_suite_t belle_sip_message_test_suite;
extern test_suite_t authentication_helper_test_suite;
extern test_suite_t belle_sip_register_test_suite;
extern test_suite_t dialog_test_suite;
extern test_suite_t refresher_test_suite;
extern test_suite_t http_test_suite;
extern test_suite_t object_test_suite;
extern char *belle_sip_tester_all_leaks_buffer;

BELLESIP_EXPORT const char *belle_sip_tester_get_root_ca_path(void);
BELLESIP_EXPORT void belle_sip_tester_set_root_ca_path(const char *root_ca_path);
BELLESIP_EXPORT void belle_sip_tester_init(void (*ftester_printf)(int level, const char *fmt, va_list args));
BELLESIP_EXPORT void belle_sip_tester_uninit(void);
BELLESIP_EXPORT int belle_sip_tester_set_log_file(const char *filename);
BELLESIP_EXPORT bool_t belle_sip_is_executable_installed(const char *executable, const char *resource);
BELLESIP_EXPORT void belle_sip_add_belr_grammar_search_path(const char *path);
BELLESIP_EXPORT void belle_sip_tester_before_each(void);
BELLESIP_EXPORT void belle_sip_tester_after_each(void);
BELLESIP_EXPORT const char *belle_sip_tester_get_root_ca_path(void);
BELLESIP_EXPORT void belle_sip_tester_set_root_ca_path(const char *root_ca_path);
BELLESIP_EXPORT void belle_sip_tester_set_dns_host_file(belle_sip_stack_t *stack);
BELLESIP_EXPORT int belle_sip_tester_ipv6_available(void);

/* WARNING: these 3 functions don't make a copy of the string. Use string litteral or ensure that their scope
 * is the entire life of the tester
 */
BELLESIP_EXPORT void belle_sip_tester_set_userhostsfile(const char *userhostfile);
BELLESIP_EXPORT void belle_sip_tester_set_test_domain(const char *domain);
BELLESIP_EXPORT void belle_sip_tester_set_auth_domain(const char *domain);

/* functions useful for any suites */
typedef enum belle_sip_tester_auth_mode {
	none,
	digest,
	digest_auth,
	digest_with_next_nonce,
	digest_auth_with_next_nonce
} belle_sip_tester_auth_mode_t;

typedef struct _belle_sip_tester_status {
	int oneHundredEighty;
	int twoHundredOk;
	int fourHundredOne;
	int fourHundredSeven;
	int fourHundredEightyOne;
	int refreshOk;
	int refreshKo;
	int dialogTerminated;
	int fiveHundredTree;
} belle_sip_tester_status_t;

typedef struct belle_sip_tester_endpoint {
	belle_sip_stack_t *stack;
	belle_sip_listener_callbacks_t *listener_callbacks;
	belle_sip_provider_t *provider;
	belle_sip_listening_point_t *lp;
	belle_sip_listener_t *listener;
	belle_sip_tester_auth_mode_t auth;
	belle_sip_tester_status_t stat;
	unsigned char expire_in_contact;
	unsigned char enable_rfc5626;
	char nonce[32];
	unsigned int nonce_count;
	const char *received;
	int expires_value;
	int rport;
	int connection_family;
	int register_count;
	int transiant_network_failure;
	belle_sip_refresher_t *refresher;
	int early_refresher;
	int number_of_body_found;
	const char *realm;
	unsigned int max_nc_count;
	const char *algo;
	const char *ha1;
	int retry_after;
	unsigned char unreconizable_contact;
	unsigned char bad_next_nonce;
	unsigned char expect_failed_auth;
	void *test_data;
} belle_sip_tester_endpoint_t;

#define BELLE_SIP_TESTER_MAX_NC_COUNT 5

BELLESIP_EXPORT belle_sip_tester_endpoint_t *belle_sip_tester_create_endpoint(
    const char *ip, int port, const char *transport, belle_sip_listener_callbacks_t *listener_callbacks);

BELLESIP_EXPORT void belle_sip_tester_generic_client_process_response_event(void *obj,
                                                                            const belle_sip_response_event_t *event);
BELLESIP_EXPORT void belle_sip_tester_destroy_endpoint(belle_sip_tester_endpoint_t *endpoint);
BELLESIP_EXPORT unsigned int
belle_sip_tester_wait_for(belle_sip_stack_t *s1, belle_sip_stack_t *s2, int *counter, int value, int timeout);

#ifdef __cplusplus
};
#endif

#endif /* _BELLE_SIP_TESTER_H */
