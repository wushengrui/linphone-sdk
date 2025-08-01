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

#include "bctoolbox/tester.h"
#include "belle-sip/belle-sip.h"
#include "belle_sip_internal.h"
#include "belle_sip_tester.h"
#include "belle_sip_tester_utils.h"

#ifndef _MSC_VER
#include "belle-sip/headers.h"
#include "belle-sip/parameters.h"
#include <stdlib.h>
#include <sys/time.h>
#endif

#define USERNAME "toto"
#define SIPDOMAIN "sip.linphone.org"
#define PASSWD "secret"
#define ALGOM "MD5"

static char publish_body[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<impp:presence xmlns:impp=\"urn:ietf:params:xml:ns:pidf entity=\"pres:someone@example.com\">\n"
    "	<impp:tuple id=\"sg89ae\">\n"
    "		<impp:status>\n"
    "			<impp:basic>open</impp:basic>\n"
    "		</impp:status>\n"
    "		<impp:contact priority=\"0.8\">tel:+09012345678</impp:contact>\n"
    "	</impp:tuple>\n"
    "</impp:presence>\n";

// static void process_dialog_terminated(void *obj, const belle_sip_dialog_terminated_event_t *event){
//	belle_sip_message("process_dialog_terminated called");
// }
// static void process_io_error(void *obj, const belle_sip_io_error_event_t *event){
//	belle_sip_warning("process_io_error");
//	/*belle_sip_main_loop_quit(belle_sip_stack_get_main_loop(stack));*/
//	/*BC_ASSERT(CU_FALSE);*/
// }

static void compute_response(const char *username,
                             const char *realm,
                             const char *passwd,
                             const char *nonce,
                             const char *method,
                             const char *uri,
                             char *response,
                             size_t size,
                             const char *algo) {
	char ha1[MAX_RESPONSE_SIZE], ha2[MAX_RESPONSE_SIZE];
	belle_sip_auth_helper_compute_ha1_for_algorithm(username, realm, passwd, ha1, size, algo);
	belle_sip_auth_helper_compute_ha2_for_algorithm(method, uri, ha2, size, algo);
	belle_sip_auth_helper_compute_response_for_algorithm(ha1, nonce, ha2, response, size, algo);
}

static void compute_response_auth_qop(const char *username,
                                      const char *realm,
                                      const char *passwd,
                                      const char *nonce,
                                      unsigned int nonce_count,
                                      const char *cnonce,
                                      const char *qop,
                                      const char *method,
                                      const char *uri,
                                      char *response,
                                      size_t size,
                                      const char *algo) {
	char ha1[MAX_RESPONSE_SIZE], ha2[MAX_RESPONSE_SIZE];
	belle_sip_auth_helper_compute_ha1_for_algorithm(username, realm, passwd, ha1, size, algo);
	belle_sip_auth_helper_compute_ha2_for_algorithm(method, uri, ha2, size, algo);
	belle_sip_auth_helper_compute_response_qop_auth_for_algorithm(ha1, nonce, nonce_count, cnonce, qop, ha2, response,
	                                                              size, algo);
}

static void server_process_request_event(void *obj, const belle_sip_request_event_t *event) {
	belle_sip_tester_endpoint_t *endpoint = (belle_sip_tester_endpoint_t *)obj;
	belle_sip_server_transaction_t *server_transaction =
	    belle_sip_provider_create_server_transaction(endpoint->provider, belle_sip_request_event_get_request(event));
	belle_sip_request_t *req = belle_sip_transaction_get_request(BELLE_SIP_TRANSACTION(server_transaction));
	belle_sip_response_t *resp;
	belle_sip_header_contact_t *contact;
	belle_sip_header_expires_t *expires;
	belle_sip_header_authorization_t *authorization;
	belle_sip_header_via_t *via;
	const char *raw_authenticate_digest;
	const char *raw_proxy_authenticate_digest;
	if ((endpoint->algo == NULL) || (!strcmp(endpoint->algo, "MD5")) || (!strcmp(endpoint->algo, "MD_SHA"))) {
		raw_authenticate_digest = "WWW-Authenticate: Digest "
		                          "algorithm=MD5, realm=\"" SIPDOMAIN "\", opaque=\"1bc7f9097684320\"";
		raw_proxy_authenticate_digest = "Proxy-Authenticate: Digest "
		                                "algorithm=MD5, realm=\"" SIPDOMAIN "\", opaque=\"1bc7f9097684320\"";
	} else if (!strcmp(endpoint->algo, "SHA-256")) {
		raw_authenticate_digest = "WWW-Authenticate: Digest "
		                          "algorithm=SHA-256, realm=\"" SIPDOMAIN "\", opaque=\"1bc7f9097684320\"";
		raw_proxy_authenticate_digest = "Proxy-Authenticate: Digest "
		                                "algorithm=SHA-256, realm=\"" SIPDOMAIN "\", opaque=\"1bc7f9097684320\"";
	} else {
		belle_sip_error("Algorithm of server must be MD5, SHA-256 or MD_SHA, can not be %s", endpoint->algo);
		return;
	}

	belle_sip_header_www_authenticate_t *www_authenticate = NULL;
	belle_sip_header_www_authenticate_t *two_www_authenticate = NULL;
	belle_sip_header_retry_after_t *retry_after_header = NULL;
	const char *auth_uri;
	const char *qop;
	unsigned char auth_ok = 0;
	size_t size;
	const char *algo;
	const char *algo_ref = "SHA-256";

	belle_sip_message("caller_process_request_event received [%s] message",
	                  belle_sip_request_get_method(belle_sip_request_event_get_request(event)));

	if (endpoint->retry_after) {
		unsigned int response_code;
		response_code = 404;
		resp = belle_sip_response_create_from_request(belle_sip_request_event_get_request(event), response_code);
		if (www_authenticate) {
			if ((endpoint->algo != NULL) && (!strcmp(endpoint->algo, "MD_SHA"))) {
				two_www_authenticate =
				    BELLE_SIP_HEADER_WWW_AUTHENTICATE(belle_sip_object_clone(BELLE_SIP_OBJECT(www_authenticate)));
				belle_sip_header_www_authenticate_set_algorithm(two_www_authenticate, algo_ref);
				belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(two_www_authenticate));
			}
			belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(www_authenticate));
		}
		retry_after_header = BELLE_SIP_HEADER_RETRY_AFTER(belle_sip_header_retry_after_create(endpoint->retry_after));

		belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(retry_after_header));

		endpoint->retry_after = 0;
	} else {
		switch (endpoint->auth) {
			case none: {
				auth_ok = 1;
				break;
			}
			case digest_auth:
			case digest_with_next_nonce:
			case digest_auth_with_next_nonce:
			case digest: {
				if ((authorization = belle_sip_message_get_header_by_type(req, belle_sip_header_authorization_t)) !=
				        NULL ||
				    (authorization = BELLE_SIP_HEADER_AUTHORIZATION(
				         belle_sip_message_get_header_by_type(req, belle_sip_header_proxy_authorization_t))) != NULL) {
					qop = belle_sip_header_authorization_get_qop(authorization);
					if (strcasecmp("REGISTER", belle_sip_request_get_method(req)) == 0) {
						BC_ASSERT_FALSE(
						    BELLE_SIP_OBJECT_IS_INSTANCE_OF(authorization, belle_sip_header_proxy_authorization_t));
					} else {
						BC_ASSERT_TRUE(
						    BELLE_SIP_OBJECT_IS_INSTANCE_OF(authorization, belle_sip_header_proxy_authorization_t));
					}
					algo = belle_sip_header_authorization_get_algorithm(authorization);
					size = belle_sip_auth_define_size(algo);
					if (!size) {
						belle_sip_error("Algorithm [%s] is not correct ", algo);
						return;
					}
					char local_resp[MAX_RESPONSE_SIZE];
					if (qop && strcmp(qop, "auth") == 0) {
						compute_response_auth_qop(
						    belle_sip_header_authorization_get_username(authorization),
						    belle_sip_header_authorization_get_realm(authorization), PASSWD, endpoint->nonce,
						    endpoint->nonce_count, belle_sip_header_authorization_get_cnonce(authorization),
						    belle_sip_header_authorization_get_qop(authorization), belle_sip_request_get_method(req),
						    auth_uri = belle_sip_uri_to_string(belle_sip_header_authorization_get_uri(authorization)),
						    local_resp, size, algo);
					} else {
						/*digest*/
						compute_response(
						    belle_sip_header_authorization_get_username(authorization),
						    belle_sip_header_authorization_get_realm(authorization), PASSWD, endpoint->nonce,
						    belle_sip_request_get_method(req),
						    auth_uri = belle_sip_uri_to_string(belle_sip_header_authorization_get_uri(authorization)),
						    local_resp, size, algo);
					}
					belle_sip_free((void *)auth_uri);
					auth_ok = strcmp(belle_sip_header_authorization_get_response(authorization), local_resp) == 0;
				}
				if (auth_ok &&
				    endpoint->nonce_count < endpoint->max_nc_count) { /*revoke nonce after MAX_NC_COUNT uses*/
					if (endpoint->auth == digest || endpoint->auth == digest_with_next_nonce ||
					    endpoint->auth == digest_auth_with_next_nonce) {
						sprintf(endpoint->nonce, "%p", authorization); //*change the nonce for next auth*/
					} else {
						endpoint->nonce_count++;
					}
				} else {
					auth_ok = 0;
					if (strcasecmp("REGISTER", belle_sip_request_get_method(req)) == 0) {
						www_authenticate = belle_sip_header_www_authenticate_parse(raw_authenticate_digest);
					} else {
						www_authenticate = BELLE_SIP_HEADER_WWW_AUTHENTICATE(
						    belle_sip_header_proxy_authenticate_parse(raw_proxy_authenticate_digest));
					}
					sprintf(endpoint->nonce, "%p", www_authenticate); //*change the nonce for next auth*/
					belle_sip_header_www_authenticate_set_nonce(www_authenticate, endpoint->nonce);
					if (endpoint->auth == digest_auth || endpoint->auth == digest_auth_with_next_nonce) {
						belle_sip_header_www_authenticate_add_qop(www_authenticate, "auth");
						if (endpoint->nonce_count >= BELLE_SIP_TESTER_MAX_NC_COUNT) {
							belle_sip_header_www_authenticate_set_stale(www_authenticate, 1);
						}
						endpoint->nonce_count = 1;
					}
				}
			} break;
			default:
				break;
		}
		if (auth_ok) {
			resp = belle_sip_response_create_from_request(belle_sip_request_event_get_request(event), 200);
			if (!endpoint->expire_in_contact) {
				belle_sip_message_add_header(
				    BELLE_SIP_MESSAGE(resp),
				    BELLE_SIP_HEADER(expires = belle_sip_message_get_header_by_type(req, belle_sip_header_expires_t)));
			}
			if (strcmp(belle_sip_request_get_method(req), "REGISTER") == 0) {
				contact = belle_sip_message_get_header_by_type(req, belle_sip_header_contact_t);
			} else {
				contact = belle_sip_header_contact_new();
			}
			if (endpoint->enable_rfc5626)
				belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp),
				                             BELLE_SIP_HEADER(belle_sip_header_supported_create("outbound")));
			if (endpoint->unreconizable_contact) {
				/*put an unexpected address*/
				belle_sip_uri_set_host(belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(contact)),
				                       "nimportequoi.com");
			}
			if (endpoint->auth == digest_with_next_nonce || endpoint->auth == digest_auth_with_next_nonce) {
				belle_sip_header_t *authentication_info;
				if (!endpoint->bad_next_nonce) {
					authentication_info = BELLE_SIP_HEADER(belle_sip_header_authentication_info_new());
					belle_sip_header_authentication_info_set_next_nonce(
					    BELLE_SIP_HEADER_AUTHENTICATION_INFO(authentication_info), endpoint->nonce);
				} else {
					authentication_info = BELLE_SIP_HEADER(belle_sip_header_extension_create(
					    BELLE_SIP_AUTHENTICATION_INFO, "nonce=\"nimporte quoi\" nc="));
				}

				belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(authentication_info));
			}
			belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(contact));
			if (strcmp(belle_sip_request_get_method(req), "PUBLISH") == 0) {

				belle_sip_header_t *sip_if_match =
				    belle_sip_message_get_header(BELLE_SIP_MESSAGE(resp), "SIP-If-Match");
				if (sip_if_match) {
					BC_ASSERT_STRING_EQUAL(
					    belle_sip_header_extension_get_value(BELLE_SIP_HEADER_EXTENSION(sip_if_match)), "blablietag");
				}
				/*check for body*/
				BC_ASSERT_PTR_NOT_NULL(belle_sip_message_get_body(BELLE_SIP_MESSAGE(req)));
				if (belle_sip_message_get_body(BELLE_SIP_MESSAGE(req))) {
					BC_ASSERT_STRING_EQUAL(belle_sip_message_get_body(BELLE_SIP_MESSAGE(req)), publish_body);
				}
				BC_ASSERT_PTR_NOT_NULL(belle_sip_message_get_header_by_type(req, belle_sip_header_content_type_t));
				BC_ASSERT_PTR_NOT_NULL(belle_sip_message_get_header_by_type(req, belle_sip_header_content_length_t));
				belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp),
				                             belle_sip_header_create("SIP-ETag", "blablietag"));
			}
			if (strcmp(belle_sip_request_get_method(req), "SUBSCRIBE") == 0) {
				if (belle_sip_request_event_get_dialog(event) == NULL) {
					belle_sip_message("creating dialog for incoming SUBSCRIBE");
					belle_sip_provider_create_dialog(endpoint->provider, BELLE_SIP_TRANSACTION(server_transaction));
				}
			}
		} else {
			unsigned int response_code;
			if (strcasecmp("REGISTER", belle_sip_request_get_method(req)) == 0) {
				response_code = 401;
			} else {
				response_code = 407;
			}
			resp = belle_sip_response_create_from_request(belle_sip_request_event_get_request(event), response_code);
			if (www_authenticate) {
				if ((endpoint->algo != NULL) && (!strcmp(endpoint->algo, "MD_SHA"))) {
					two_www_authenticate =
					    BELLE_SIP_HEADER_WWW_AUTHENTICATE(belle_sip_object_clone(BELLE_SIP_OBJECT(www_authenticate)));
					belle_sip_header_www_authenticate_set_algorithm(two_www_authenticate, algo_ref);
					belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(two_www_authenticate));
				}
				belle_sip_message_add_header(BELLE_SIP_MESSAGE(resp), BELLE_SIP_HEADER(www_authenticate));
			}
		}
	}

	if (endpoint->received) {
		via = belle_sip_message_get_header_by_type(req, belle_sip_header_via_t);
		belle_sip_header_via_set_received(via, endpoint->received);
	}
	if (belle_sip_message_get_body(BELLE_SIP_MESSAGE(req))) {
		endpoint->number_of_body_found++;
	}
	belle_sip_server_transaction_send_response(server_transaction, resp);
}

static void client_process_dialog_terminated(void *obj, const belle_sip_dialog_terminated_event_t *event) {
	belle_sip_tester_endpoint_t *endpoint = (belle_sip_tester_endpoint_t *)obj;
	endpoint->stat.dialogTerminated++;
}

static void server_process_dialog_terminated(void *obj, const belle_sip_dialog_terminated_event_t *event) {
	belle_sip_tester_endpoint_t *endpoint = (belle_sip_tester_endpoint_t *)obj;
	endpoint->stat.dialogTerminated++;
}

// static void process_timeout(void *obj, const belle_sip_timeout_event_t *event){
//	belle_sip_message("process_timeout");
// }
// static void process_transaction_terminated(void *obj, const belle_sip_transaction_terminated_event_t *event){
//	belle_sip_message("process_transaction_terminated");
// }

static void client_process_auth_requested(void *obj, belle_sip_auth_event_t *event) {
	belle_sip_tester_endpoint_t *endpoint = (belle_sip_tester_endpoint_t *)obj;
	if (endpoint->algo == NULL) endpoint->algo = "MD5";
	belle_sip_message("process_auth_requested requested for [%s@%s]", belle_sip_auth_event_get_username(event),
	                  belle_sip_auth_event_get_realm(event));
	if (((event->algorithm) && (!strcmp(endpoint->algo, event->algorithm)))) {
		if (endpoint->ha1) belle_sip_auth_event_set_ha1(event, endpoint->ha1);
		else belle_sip_auth_event_set_passwd(event, PASSWD);
	}
}

static void belle_sip_refresher_listener(belle_sip_refresher_t *refresher,
                                         void *user_pointer,
                                         unsigned int status_code,
                                         const char *reason_phrase,
                                         int will_retry) {
	belle_sip_tester_endpoint_t *endpoint = (belle_sip_tester_endpoint_t *)user_pointer;
	BELLESIP_UNUSED(refresher);
	belle_sip_message("belle_sip_refresher_listener [%i] reason [%s]", status_code, reason_phrase);
	if (status_code >= 300) endpoint->stat.refreshKo++;
	switch (status_code) {
		case 200:
			endpoint->stat.refreshOk++;
			break;
		case 481:
			endpoint->stat.fourHundredEightyOne++;
			break;
		case 503:
			endpoint->stat.fiveHundredTree++;
			break;
		case 401:
			endpoint->stat.fourHundredOne++;
			break;
		default:
			/*nop*/
			break;
	}
	if (endpoint->stat.refreshKo == 1 && endpoint->transiant_network_failure) {
		belle_sip_stack_set_send_error(endpoint->stack, 0);
	} else if (endpoint->stat.refreshOk == 1 && endpoint->stat.refreshKo == 0 && endpoint->transiant_network_failure) {
		/*generate a network failure*/
		belle_sip_refresher_set_retry_after(endpoint->refresher, 100);
		belle_sip_stack_set_send_error(endpoint->stack, -1);
	}
}

static belle_sip_tester_endpoint_t *create_udp_endpoint(int port, belle_sip_listener_callbacks_t *listener_callbacks) {
	belle_sip_tester_endpoint_t *endpoint =
	    belle_sip_tester_create_endpoint("127.0.0.1", port, "udp", listener_callbacks);
	BC_ASSERT_PTR_NOT_NULL(endpoint->lp);
	return endpoint;
}

static belle_sip_tester_endpoint_t *create_tcp_endpoint(int port, belle_sip_listener_callbacks_t *listener_callbacks) {
	belle_sip_tester_endpoint_t *endpoint =
	    belle_sip_tester_create_endpoint("127.0.0.1", port, "tcp", listener_callbacks);
	BC_ASSERT_PTR_NOT_NULL(endpoint->lp);
	return endpoint;
}

static belle_sip_refresher_t *refresher_base_with_body2(belle_sip_tester_endpoint_t *client,
                                                        belle_sip_tester_endpoint_t *server,
                                                        const char *method,
                                                        belle_sip_header_content_type_t *content_type,
                                                        const char *body,
                                                        int number_active_refresher) {
	belle_sip_request_t *req;
	belle_sip_client_transaction_t *trans;
	belle_sip_header_route_t *destination_route;
	belle_sip_refresher_t *refresher = NULL;
	const char *identity = "sip:" USERNAME "@" SIPDOMAIN;
	const char *domain = "sip:" SIPDOMAIN;
	belle_sip_header_contact_t *contact = belle_sip_header_contact_new();
	belle_sip_uri_t *dest_uri;
	uint64_t begin;
	uint64_t end;
	if (client->expire_in_contact) belle_sip_header_contact_set_expires(contact, client->expires_value);

	dest_uri =
	    (belle_sip_uri_t *)belle_sip_object_clone((belle_sip_object_t *)belle_sip_listening_point_get_uri(server->lp));
	if (client->connection_family == AF_INET6) belle_sip_uri_set_host(dest_uri, "::1");
	else belle_sip_uri_set_host(dest_uri, "127.0.0.1");
	destination_route = belle_sip_header_route_create(belle_sip_header_address_create(NULL, dest_uri));

	req = belle_sip_request_create(
	    belle_sip_uri_parse(domain), method, belle_sip_provider_create_call_id(client->provider),
	    belle_sip_header_cseq_create(20, method), belle_sip_header_from_create2(identity, BELLE_SIP_RANDOM_TAG),
	    belle_sip_header_to_create2(identity, NULL), belle_sip_header_via_new(), 70);
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(contact));
	if (client->enable_rfc5626)
		belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),
		                             BELLE_SIP_HEADER(belle_sip_header_supported_create("outbound")));
	if (!client->expire_in_contact)
		belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),
		                             BELLE_SIP_HEADER(belle_sip_header_expires_create(client->expires_value)));

	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(destination_route));
	if (content_type && body) {
		size_t body_lenth = strlen(body);
		belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(content_type));
		belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),
		                             BELLE_SIP_HEADER(belle_sip_header_content_length_create(body_lenth)));
		belle_sip_message_set_body(BELLE_SIP_MESSAGE(req), body, body_lenth);
	}
	if (client->realm && belle_sip_provider_add_authorization(client->provider, req, NULL, NULL, NULL, client->realm)) {
	}
	trans = belle_sip_provider_create_client_transaction(client->provider, req);

	belle_sip_object_ref(trans); /*to avoid trans from being deleted before refresher can use it*/
	belle_sip_client_transaction_send_request(trans);
	if (client->early_refresher) {
		client->refresher = refresher = belle_sip_client_transaction_create_refresher(trans);
		if (client->realm) belle_sip_refresher_set_realm(client->refresher, client->realm);
	} else {
		if (server->auth == none) {
			BC_ASSERT_TRUE(
			    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.twoHundredOk, 1, 1000));
		} else {
			if (strcasecmp("REGISTER", belle_sip_request_get_method(req)) == 0) {
				BC_ASSERT_TRUE(
				    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredOne, 1, 1000));
			} else {
				BC_ASSERT_TRUE(
				    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredSeven, 1, 1000));
			}
			/*update cseq*/
			req = belle_sip_client_transaction_create_authenticated_request(trans, NULL, NULL);
			belle_sip_object_unref(trans);
			trans = belle_sip_provider_create_client_transaction(client->provider, req);
			belle_sip_object_ref(trans);
			belle_sip_client_transaction_send_request(trans);
			if (client->expect_failed_auth) {
				BC_ASSERT_TRUE(
				    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredOne, 1, 1000));
			} else {
				BC_ASSERT_TRUE(
				    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.twoHundredOk, 1, 1000));
			}
		}
		client->refresher = refresher = belle_sip_client_transaction_create_refresher(trans);
	}
	if (BC_ASSERT_PTR_NOT_NULL(refresher)) {
		belle_sip_object_unref(trans);
		belle_sip_refresher_set_listener(refresher, belle_sip_refresher_listener, client);
		int timeout = client->retry_after + client->register_count;

		if (!client->expect_failed_auth) {
			begin = belle_sip_time_ms();
			BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk,
			                                         client->register_count + (client->early_refresher ? 1 : 0),
			                                         timeout * 1000 + 1000));
			end = belle_sip_time_ms();
			BC_ASSERT_GREATER((long double)(end - begin),
			                  (client->register_count / number_active_refresher) * 1000 * .9, long double,
			                  "%Lf"); /*because refresh is at 90% of expire*/
			BC_ASSERT_LOWER_STRICT(end - begin, (timeout * 1000 + 2000), unsigned long long, "%llu");
		} else {
			/* Will retry one more time before re-scheduling an attempt in 60 seconds */
			BC_ASSERT_TRUE(
			    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredOne, 2, 3000));
		}
	}
	return refresher;
}

static void refresher_base_with_body(belle_sip_tester_endpoint_t *client,
                                     belle_sip_tester_endpoint_t *server,
                                     const char *method,
                                     belle_sip_header_content_type_t *content_type,
                                     const char *body) {
	belle_sip_refresher_t *refresher = refresher_base_with_body2(client, server, method, content_type, body, 1);
	int fourHundredOneResponses = client->stat.fourHundredOne;

	belle_sip_refresher_refresh(refresher, 0);
	if (!client->expect_failed_auth) {
		/*unregister twice to make sure refresh operation can be safely cascaded*/
		belle_sip_refresher_refresh(refresher, 0);
		BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk,
		                                         client->register_count + 1, 1000));
		BC_ASSERT_EQUAL(client->stat.refreshOk, client->register_count + 1, int, "%d");
	} else {
		BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredOne,
		                                         fourHundredOneResponses + 1, 2000));
	}
	belle_sip_refresher_stop(refresher);
	belle_sip_object_unref(refresher);
}

static void
refresher_base(belle_sip_tester_endpoint_t *client, belle_sip_tester_endpoint_t *server, const char *method) {
	refresher_base_with_body(client, server, method, NULL, NULL);
}
static void register_base(belle_sip_tester_endpoint_t *client, belle_sip_tester_endpoint_t *server) {
	refresher_base(client, server, "REGISTER");
}
static void refresher_base_with_param_and_body_for_ha1(const char *method,
                                                       unsigned char expire_in_contact,
                                                       belle_sip_tester_auth_mode_t auth_mode,
                                                       int early_refresher,
                                                       belle_sip_header_content_type_t *content_type,
                                                       const char *body,
                                                       const char *client_algo,
                                                       const char *server_algo,
                                                       const char *ha1) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = client->expire_in_contact = expire_in_contact;
	server->auth = auth_mode;
	server->algo = server_algo;
	client->algo = client_algo;
	client->early_refresher = early_refresher;
	if (ha1) {
		client->ha1 = ha1;
	}
	refresher_base_with_body(client, server, method, content_type, body);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void refresher_base_with_param_and_body(const char *method,
                                               unsigned char expire_in_contact,
                                               belle_sip_tester_auth_mode_t auth_mode,
                                               int early_refresher,
                                               belle_sip_header_content_type_t *content_type,
                                               const char *body,
                                               const char *client_algo,
                                               const char *server_algo) {
	refresher_base_with_param_and_body_for_ha1(method, expire_in_contact, auth_mode, early_refresher, content_type,
	                                           body, client_algo, server_algo, NULL);
}

static void refresher_base_with_param(const char *method,
                                      unsigned char expire_in_contact,
                                      belle_sip_tester_auth_mode_t auth_mode,
                                      const char *client_algo,
                                      const char *server_algo,
                                      const char *ha1) {
	refresher_base_with_param_and_body_for_ha1(method, expire_in_contact, auth_mode, FALSE, NULL, NULL, client_algo,
	                                           server_algo, ha1);
}

static void register_test_with_param_for_algorithm(unsigned char expire_in_contact,
                                                   belle_sip_tester_auth_mode_t auth_mode,
                                                   const char *client_algo,
                                                   const char *server_algo) {
	refresher_base_with_param("REGISTER", expire_in_contact, auth_mode, client_algo, server_algo, NULL);
}

static void register_test_with_param_for_algorithm_ha1(unsigned char expire_in_contact,
                                                       belle_sip_tester_auth_mode_t auth_mode,
                                                       const char *client_algo,
                                                       const char *server_algo,
                                                       const char *ha1) {
	refresher_base_with_param("REGISTER", expire_in_contact, auth_mode, client_algo, server_algo, ha1);
}

static void register_test_with_param(unsigned char expire_in_contact, belle_sip_tester_auth_mode_t auth_mode) {
	register_test_with_param_for_algorithm(expire_in_contact, auth_mode, NULL, NULL);
}

static char *list = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<resource-lists xmlns=\"urn:ietf:params:xml:ns:resource-lists\"\n"
                    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
                    "<list>\n"
                    "\t<entry uri=\"sip:marie@sexample.com\" />\n"
                    "\t<entry uri=\"sip:pauline@sexample.com\" />\n"
                    "</list>\n"
                    "</resource-lists>\n";

static void subscribe_base(int with_resource_lists) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_request_t *req;
	belle_sip_client_transaction_t *trans;
	belle_sip_header_route_t *destination_route;
	const char *identity = "sip:" USERNAME "@" SIPDOMAIN;
	const char *domain = "sip:" SIPDOMAIN;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_uri_t *dest_uri;
	belle_sip_refresher_t *refresher;
	belle_sip_header_contact_t *contact = belle_sip_header_contact_new();
	belle_sip_dialog_t *client_dialog;
	char *call_id = NULL;
	int dummy = 0;
	uint64_t begin;
	uint64_t end;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));

	client_callbacks.process_dialog_terminated = client_process_dialog_terminated;
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	server_callbacks.process_dialog_terminated = server_process_dialog_terminated;

	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 0;
	server->auth = digest_auth;

	dest_uri =
	    (belle_sip_uri_t *)belle_sip_object_clone((belle_sip_object_t *)belle_sip_listening_point_get_uri(server->lp));
	belle_sip_uri_set_host(dest_uri, "127.0.0.1");
	destination_route = belle_sip_header_route_create(belle_sip_header_address_create(NULL, dest_uri));

	req = belle_sip_request_create(
	    belle_sip_uri_parse(domain), "SUBSCRIBE", belle_sip_provider_create_call_id(client->provider),
	    belle_sip_header_cseq_create(20, "SUBSCRIBE"), belle_sip_header_from_create2(identity, BELLE_SIP_RANDOM_TAG),
	    belle_sip_header_to_create2(identity, NULL), belle_sip_header_via_new(), 70);
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(contact));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(belle_sip_header_expires_create(1)));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),
	                             BELLE_SIP_HEADER(belle_sip_header_create("Event", "Presence")));

	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(destination_route));
	if (with_resource_lists) {
		belle_sip_message_add_header(BELLE_SIP_MESSAGE(req), BELLE_SIP_HEADER(belle_sip_header_content_type_create(
		                                                         "application", "resource-lists+xml")));
		belle_sip_message_set_body(BELLE_SIP_MESSAGE(req), list, strlen(list));
	}

	trans = belle_sip_provider_create_client_transaction(client->provider, req);
	belle_sip_object_ref(trans); /*to avoid trans from being deleted before refresher can use it*/
	belle_sip_client_transaction_send_request(trans);

	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredSeven, 1, 1000));

	req = belle_sip_client_transaction_create_authenticated_request(trans, NULL, NULL);
	belle_sip_object_unref(trans);
	trans = belle_sip_provider_create_client_transaction(client->provider, req);
	belle_sip_object_ref(trans);
	belle_sip_client_transaction_send_request(trans);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.twoHundredOk, 1, 1000));
	/*maybe dialog should be automatically created*/
	BC_ASSERT_PTR_NOT_NULL(client_dialog = belle_sip_transaction_get_dialog(BELLE_SIP_TRANSACTION(trans)));
	call_id = belle_sip_strdup(belle_sip_header_call_id_get_call_id(belle_sip_dialog_get_call_id(client_dialog)));
	belle_sip_object_ref(client_dialog);

	refresher = belle_sip_client_transaction_create_refresher(trans);
	belle_sip_object_unref(trans);
	belle_sip_refresher_set_listener(refresher, belle_sip_refresher_listener, client);

	begin = belle_sip_time_ms();
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk, 3, 4000));
	end = belle_sip_time_ms();
	BC_ASSERT_GREATER((long double)(end - begin), 3000 * .9, long double, "%Lf");
	BC_ASSERT_LOWER_STRICT(end - begin, 5000, unsigned long long, "%llu");

	belle_sip_message("simulating dialog error and recovery");
	belle_sip_stack_set_send_error(client->stack, 1);
	BC_ASSERT_TRUE(
	    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredEightyOne, 1, 4000));
	/*let the transaction timeout*/
	belle_sip_tester_wait_for(server->stack, client->stack, &dummy, 1, 32000);
	belle_sip_stack_set_send_error(client->stack, 0);

	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk, 4, 4000));
	BC_ASSERT_EQUAL(client->stat.dialogTerminated, 0, int, "%i");

	/*make sure dialog has changed*/
	BC_ASSERT_PTR_NOT_EQUAL(client_dialog, belle_sip_transaction_get_dialog(
	                                           BELLE_SIP_TRANSACTION(belle_sip_refresher_get_transaction(refresher))));
	BC_ASSERT_STRING_NOT_EQUAL(
	    call_id, belle_sip_header_call_id_get_call_id(belle_sip_dialog_get_call_id(belle_sip_transaction_get_dialog(
	                 BELLE_SIP_TRANSACTION(belle_sip_refresher_get_transaction(refresher))))));
	belle_sip_free(call_id);
	belle_sip_object_unref(client_dialog);

	belle_sip_message("simulating dialog terminated server side and recovery");

	client_dialog =
	    belle_sip_transaction_get_dialog(BELLE_SIP_TRANSACTION(belle_sip_refresher_get_transaction(refresher)));
	call_id = belle_sip_strdup(belle_sip_header_call_id_get_call_id(belle_sip_dialog_get_call_id(client_dialog)));
	belle_sip_object_ref(client_dialog);

	belle_sip_provider_enable_unconditional_answer(server->provider, TRUE);
	belle_sip_provider_set_unconditional_answer(server->provider, 481);
	belle_sip_refresher_refresh(refresher, 10);

	BC_ASSERT_TRUE(
	    belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.fourHundredEightyOne, 2, 4000));
	belle_sip_provider_enable_unconditional_answer(server->provider, FALSE);

	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk, 5, 4000));
	BC_ASSERT_EQUAL(client->stat.dialogTerminated, 0, int, "%i");

	/*make sure dialog has changed*/
	BC_ASSERT_PTR_NOT_EQUAL(client_dialog, belle_sip_transaction_get_dialog(
	                                           BELLE_SIP_TRANSACTION(belle_sip_refresher_get_transaction(refresher))));
	BC_ASSERT_STRING_NOT_EQUAL(
	    call_id, belle_sip_header_call_id_get_call_id(belle_sip_dialog_get_call_id(belle_sip_transaction_get_dialog(
	                 BELLE_SIP_TRANSACTION(belle_sip_refresher_get_transaction(refresher))))));
	belle_sip_free(call_id);
	belle_sip_object_unref(client_dialog);
	belle_sip_refresher_refresh(refresher, 0);
	belle_sip_refresher_refresh(refresher, 0);

	belle_sip_refresher_stop(refresher);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &server->stat.dialogTerminated, 3, 4000));

	belle_sip_object_unref(refresher);

	if (with_resource_lists) {
		BC_ASSERT_EQUAL(server->number_of_body_found, (server->auth == none ? 3 : 6), int, "%i");
	}

	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void subscribe_test(void) {
	subscribe_base(FALSE);
}
static void subscribe_list_test(void) {
	subscribe_base(TRUE);
}

static void register_expires_header(void) {
	register_test_with_param(0, none);
}

static void register_expires_in_contact(void) {
	register_test_with_param(1, none);
}

static void register_expires_header_digest(void) {
	register_test_with_param(0, digest);
}

static void register_expires_in_contact_header_digest_auth(void) {
	register_test_with_param(1, digest_auth);
}

static void register_expires_in_contact_header_digest_auth_sha256(void) {
	register_test_with_param_for_algorithm(1, digest_auth, "SHA-256", "SHA-256");
}

static void register_expires_in_contact_header_digest_md_sha(void) {
	register_test_with_param_for_algorithm(1, digest_auth, "MD5", "MD_SHA");
}

static void register_expires_in_contact_header_digest_md_sha_ha1(void) {
	register_test_with_param_for_algorithm_ha1(1, digest_auth, "MD5", "MD_SHA", "323897f425eb3f4c22efa4119c85b434");
}

static void register_expires_in_contact_header_digest_md_sha256(void) {
	register_test_with_param_for_algorithm(1, digest_auth, "SHA-256", "MD_SHA");
}

static void register_expires_in_contact_header_digest_md_sha256_ha1(void) {
	register_test_with_param_for_algorithm_ha1(1, digest_auth, "SHA-256", "MD_SHA",
	                                           "ebf7a06f8211417d6735cd8bfcbecc30ef2045a80d1d6148a6b9d5d3c4f76911");
}

static void register_with_failure(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	client->transiant_network_failure = 1;
	register_base(client, server);
	BC_ASSERT_EQUAL(client->stat.refreshKo, 1, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_with_pingpong_pong_ok(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_refresher_t *refresher;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_tcp_endpoint(3452, &client_callbacks);
	client->enable_rfc5626 = 1;
	client->early_refresher = 1;
	client->register_count = 0;
	client->expires_value = 60;
	server = create_tcp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	server->enable_rfc5626 = 1;

	belle_sip_stack_set_pong_timeout(client->stack, 4);

	refresher = refresher_base_with_body2(client, server, "REGISTER", NULL, NULL, 1);

	/* send a client keepalive (PING)*/
	belle_sip_listening_point_send_keep_alive(client->lp);
	belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 1000);
	/* server sends a keepalive (PONG)*/
	belle_sip_listening_point_send_pong(server->lp);
	belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 6000);
	BC_ASSERT_EQUAL(client->stat.refreshOk, 1, int, "%d");

	belle_sip_refresher_stop(refresher);
	belle_sip_object_unref(refresher);

	BC_ASSERT_EQUAL(client->stat.refreshKo, 0, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_with_pingpong_pong_nok_base(bool_t ping_pong_verification) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_refresher_t *refresher;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_tcp_endpoint(3452, &client_callbacks);
	client->early_refresher = 1;
	client->register_count = 0;
	client->expires_value = 60;
	client->enable_rfc5626 = 1;
	server = create_tcp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	server->enable_rfc5626 = 1;

	belle_sip_stack_set_pong_timeout(client->stack, 4);
	belle_sip_stack_enable_ping_pong_verification(client->stack, ping_pong_verification);

	refresher = refresher_base_with_body2(client, server, "REGISTER", NULL, NULL, 1);

	/* The first PING-PONG is needed by belle-sip to know whether the other party supports RFC5626.
	 * If property pong_support_confirmed is set to TRUE, there is no need of the first pong to simulate a broken
	 * connection */
	if (!!ping_pong_verification) {
		/* send a client keepalive (PING)*/
		belle_sip_listening_point_send_keep_alive(client->lp);
		belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 1000);
		/* the server replies with PONG.*/
		belle_sip_listening_point_send_pong(server->lp);
		belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 1000);
	}

	/* send another client keepalive (PING)*/
	belle_sip_listening_point_send_keep_alive(client->lp);
	/* we simulate a broken connection, no PONG is sent by server*/
	belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 6000);
	/* as a result the channel should have been closed and a new REGISTER sent */
	BC_ASSERT_EQUAL(client->stat.refreshOk, 2, int, "%d");

	belle_sip_refresher_stop(refresher);
	belle_sip_object_unref(refresher);

	BC_ASSERT_EQUAL(client->stat.refreshKo, 1, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_with_pingpong_pong_nok(void) {
	register_with_pingpong_pong_nok_base(TRUE);
}

static void register_with_pingpong_without_pingpong_verification(void) {
	register_with_pingpong_pong_nok_base(FALSE);
}

static void register_with_pingpong_not_supported(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_refresher_t *refresher;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_tcp_endpoint(3452, &client_callbacks);
	client->enable_rfc5626 = 1;
	client->early_refresher = 1;
	client->register_count = 0;
	client->expires_value = 60;
	server = create_tcp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	server->enable_rfc5626 = 0;

	belle_sip_stack_set_pong_timeout(client->stack, 4);

	refresher = refresher_base_with_body2(client, server, "REGISTER", NULL, NULL, 1);

	/* send a client keepalive (PING)*/
	belle_sip_listening_point_send_keep_alive(client->lp);
	/* server does not send a PONG, but nothing should happen at client side.*/
	belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 6000);
	BC_ASSERT_EQUAL(client->stat.refreshOk, 1, int, "%d");

	belle_sip_refresher_stop(refresher);
	belle_sip_object_unref(refresher);

	BC_ASSERT_EQUAL(client->stat.refreshKo, 0, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

/* Case of a server that claims Supported: outbound, but does not send any pong.
 * In this case the client does not break the connection since no pong was ever received.
 */
static void register_with_pingpong_not_supported_2(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_refresher_t *refresher;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_tcp_endpoint(3452, &client_callbacks);
	client->enable_rfc5626 = 1;
	client->early_refresher = 1;
	client->register_count = 0;
	client->expires_value = 60;
	server = create_tcp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	server->enable_rfc5626 = 0;

	belle_sip_stack_set_pong_timeout(client->stack, 4);

	refresher = refresher_base_with_body2(client, server, "REGISTER", NULL, NULL, 1);

	/* send a client keepalive (PING)*/
	belle_sip_listening_point_send_keep_alive(client->lp);
	/* server does not send a PONG, and nothing should happen at client side.*/
	belle_sip_tester_wait_for(server->stack, client->stack, NULL, 0, 6000);
	BC_ASSERT_EQUAL(client->stat.refreshOk, 1, int, "%d");

	belle_sip_refresher_stop(refresher);
	belle_sip_object_unref(refresher);

	BC_ASSERT_EQUAL(client->stat.refreshKo, 0, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_with_failure_because_of_no_qop(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_digest_authentication_policy_t *digest_policy = belle_sip_digest_authentication_policy_new();

	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;

	belle_sip_digest_authentication_policy_set_allow_md5(digest_policy, TRUE);
	belle_sip_digest_authentication_policy_set_allow_no_qop(digest_policy, FALSE);
	belle_sip_stack_set_digest_authentication_policy(client->stack, digest_policy);
	client->expect_failed_auth = TRUE;

	register_base(client, server);
	BC_ASSERT_EQUAL(client->stat.twoHundredOk, 0, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_with_failure_because_of_md5(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_digest_authentication_policy_t *digest_policy = belle_sip_digest_authentication_policy_new();

	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);

	belle_sip_digest_authentication_policy_set_allow_md5(digest_policy, FALSE);
	belle_sip_digest_authentication_policy_set_allow_no_qop(digest_policy, TRUE);
	belle_sip_stack_set_digest_authentication_policy(client->stack, digest_policy);
	client->expect_failed_auth = TRUE;

	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;

	register_base(client, server);
	BC_ASSERT_EQUAL(client->stat.twoHundredOk, 0, int, "%d");
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void refresher_sha256_with_md5_disabled(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_digest_authentication_policy_t *digest_policy = belle_sip_digest_authentication_policy_new();

	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);

	belle_sip_digest_authentication_policy_set_allow_md5(digest_policy, FALSE);
	belle_sip_digest_authentication_policy_set_allow_no_qop(digest_policy, TRUE);
	belle_sip_stack_set_digest_authentication_policy(client->stack, digest_policy);

	server = create_udp_endpoint(6788, &server_callbacks);

	server->auth = digest_auth;
	server->algo = "SHA-256";
	client->algo = "SHA-256";

	refresher_base_with_body(client, server, "REGISTER", NULL, NULL);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_with_unrecognizable_contact(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->unreconizable_contact = 1;
	server->auth = digest;
	register_base(client, server);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}
static void register_early_refresher(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	client->early_refresher = 1;
	register_base(client, server);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_retry_after(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = 1;
	server->auth = digest;
	client->early_refresher = 1;
	server->retry_after = 2;
	client->retry_after = 2;
	register_base(client, server);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static int register_test_with_interfaces(const char *transport,
                                         const char *client_ip,
                                         const char *server_ip,
                                         int connection_family) {
	int ret = 0;
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = belle_sip_tester_create_endpoint(client_ip, 3452, transport, &client_callbacks);
	client->connection_family = connection_family;
	client->register_count = 1;

	server = belle_sip_tester_create_endpoint(server_ip, 6788, transport, &server_callbacks);
	server->expire_in_contact = client->expire_in_contact = 0;
	server->auth = none;

	if (client->lp == NULL || server->lp == NULL) {
		belle_sip_warning("Cannot check ipv6 because host has no ipv6 support.");
		ret = -1;
	} else register_base(client, server);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
	return ret;
}

static int register_test_with_random_port(const char *transport,
                                          const char *client_ip,
                                          const char *server_ip,
                                          int connection_family) {
	int ret = 0;
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = belle_sip_tester_create_endpoint(client_ip, -1, transport, &client_callbacks);
	client->connection_family = connection_family;
	client->register_count = 1;

	server = belle_sip_tester_create_endpoint(server_ip, 6788, transport, &server_callbacks);
	server->expire_in_contact = client->expire_in_contact = 0;
	server->auth = none;

	if (client->lp == NULL || server->lp == NULL) {
		belle_sip_warning("Cannot check ipv6 because host has no ipv6 support.");
		ret = -1;
	} else register_base(client, server);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
	return ret;
}

static void register_test_ipv6_to_ipv4(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("udp", "::0", "0.0.0.0", AF_INET);
}

static void register_test_ipv4_to_ipv6(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("udp", "0.0.0.0", "::0", AF_INET);
}

static void register_test_ipv6_to_ipv6_with_ipv4(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("udp", "::0", "::0", AF_INET);
}

static void register_test_ipv6_to_ipv6_with_ipv6(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("udp", "::0", "::0", AF_INET6);
}

static void register_tcp_test_ipv6_to_ipv4(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("tcp", "::0", "0.0.0.0", AF_INET);
}

static void register_tcp_test_ipv4_to_ipv6(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("tcp", "0.0.0.0", "::0", AF_INET);
}

static void register_tcp_test_ipv6_to_ipv6_with_ipv4(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("tcp", "::0", "::0", AF_INET);
}

static void register_tcp_test_ipv6_to_ipv6_with_ipv6(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_interfaces("tcp", "::0", "::0", AF_INET6);
}

static void register_udp_test_ipv4_random_port(void) {
	register_test_with_random_port("udp", "0.0.0.0", "0.0.0.0", AF_INET);
}

static void register_udp_test_ipv6_random_port(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_random_port("udp", "::0", "0.0.0.0", AF_INET);
}

static void register_tcp_test_ipv4_random_port(void) {
	register_test_with_random_port("tcp", "0.0.0.0", "0.0.0.0", AF_INET);
}

static void register_tcp_test_ipv6_random_port(void) {
	if (!belle_sip_tester_ipv6_available()) {
		belle_sip_warning("Test skipped, IPv6 connectivity not available.");
		return;
	}
	register_test_with_random_port("tcp", "::0", "0.0.0.0", AF_INET);
}

static void simple_publish(void) {
	belle_sip_header_content_type_t *content_type = belle_sip_header_content_type_create("application", "pidf+xml");
	refresher_base_with_param_and_body("PUBLISH", FALSE, TRUE, FALSE, content_type, publish_body, "MD5", "MD5");
}
static void simple_publish_with_early_refresher(void) {
	belle_sip_header_content_type_t *content_type = belle_sip_header_content_type_create("application", "pidf+xml");
	refresher_base_with_param_and_body("PUBLISH", FALSE, TRUE, TRUE, content_type, publish_body, "MD5", "MD5");
}

static void register_and_publish(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_refresher_t *register_refresher;
	belle_sip_refresher_t *publish_refresher;
	belle_sip_header_content_type_t *content_type = belle_sip_header_content_type_create("application", "pidf+xml");

	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->expire_in_contact = TRUE;
	server->auth = digest_auth;
	client->early_refresher = TRUE;
	client->realm = SIPDOMAIN;
	register_refresher = refresher_base_with_body2(client, server, "REGISTER", NULL, NULL, 1);

	client->register_count = 2 * client->register_count;
	client->stat.refreshOk = 0;
	/*to make sure we can still use same nonce*/
	client->nonce_count -= 2;

	publish_refresher = refresher_base_with_body2(client, server, "PUBLISH", content_type, publish_body, 2);

	belle_sip_refresher_refresh(register_refresher, 0);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk,
	                                         client->register_count + 1, 1000));
	BC_ASSERT_EQUAL(client->stat.refreshOk, client->register_count + 1, int, "%d");
	belle_sip_refresher_stop(register_refresher);
	belle_sip_object_unref(register_refresher);

	belle_sip_refresher_refresh(publish_refresher, 0);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(server->stack, client->stack, &client->stat.refreshOk,
	                                         client->register_count + 1, 1000));
	BC_ASSERT_EQUAL(client->stat.refreshOk, client->register_count + 1, int, "%d");
	belle_sip_refresher_stop(publish_refresher);
	belle_sip_object_unref(publish_refresher);

	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static void register_digest_with_next_nonce(void) {
	register_test_with_param(0, digest_with_next_nonce);
}

static void register_digest_auth_with_next_nonce(void) {
	register_test_with_param(0, digest_auth_with_next_nonce);
}

static void register_digest_auth_with_bad_next_nonce(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = belle_sip_tester_generic_client_process_response_event;
	client_callbacks.process_auth_requested = client_process_auth_requested;
	server_callbacks.process_request_event = server_process_request_event;
	client = create_udp_endpoint(3452, &client_callbacks);
	server = create_udp_endpoint(6788, &server_callbacks);
	server->auth = digest_auth_with_next_nonce;
	server->bad_next_nonce = TRUE;
	register_base(client, server);
	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

typedef struct channel_bank_test_data {
	belle_sip_server_transaction_t *server_transaction;
	int request_count;
} channel_bank_test_data_t;

static void channel_bank_test_server_process_request(void *user_data, const belle_sip_request_event_t *ev) {
	belle_sip_tester_endpoint_t *endpoint = (belle_sip_tester_endpoint_t *)user_data;
	belle_sip_response_t *resp;
	belle_sip_server_transaction_t *tr;
	channel_bank_test_data_t *test_data = (channel_bank_test_data_t *)endpoint->test_data;
	test_data->request_count++;
	tr = belle_sip_provider_create_server_transaction(endpoint->provider, belle_sip_request_event_get_request(ev));
	resp = belle_sip_response_create_from_request(belle_sip_request_event_get_request(ev), 200);
	test_data->server_transaction = (belle_sip_server_transaction_t *)belle_sip_object_ref(tr);
	belle_sip_server_transaction_send_response(tr, resp);
}

static void channel_bank_test(void) {
	belle_sip_listener_callbacks_t client_callbacks;
	belle_sip_listener_callbacks_t server_callbacks;
	belle_sip_tester_endpoint_t *client, *server;
	belle_sip_request_t *req;
	belle_sip_client_transaction_t *client_transaction1, *client_transaction2;
	belle_sip_server_transaction_t *server_transaction1, *server_transaction2;
	channel_bank_test_data_t server_data = {0};
	channel_bank_test_data_t client_data = {0};
	belle_sip_uri_t *dest_uri;
	int client_port1;
	int client_port2;

	memset(&client_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	memset(&server_callbacks, 0, sizeof(belle_sip_listener_callbacks_t));
	client_callbacks.process_response_event = NULL;
	client_callbacks.process_request_event = channel_bank_test_server_process_request;
	client_callbacks.process_auth_requested = NULL;
	server_callbacks.process_request_event = channel_bank_test_server_process_request;
	client = create_udp_endpoint(BELLE_SIP_LISTENING_POINT_DONT_BIND, &client_callbacks);
	client->test_data = &client_data;
	server = create_udp_endpoint(6788, &server_callbacks);
	server->test_data = &server_data;

	/* send a first request */
	req = belle_sip_request_create(
	    belle_sip_uri_parse("sip:example.org"), "OPTIONS", belle_sip_provider_create_call_id(client->provider),
	    belle_sip_header_cseq_create(20, "OPTIONS"),
	    belle_sip_header_from_create2("sip:bob@example.org", BELLE_SIP_RANDOM_TAG),
	    belle_sip_header_to_create2("sip:alice@example.org", NULL), belle_sip_header_via_new(), 70);
	belle_sip_message_set_channel_bank_identifier(BELLE_SIP_MESSAGE(req), "xxx");

	client_transaction1 = belle_sip_provider_create_client_transaction(client->provider, req);
	belle_sip_client_transaction_send_request_to(client_transaction1, server->lp->listening_uri);
	belle_sip_object_ref(client_transaction1);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(client->stack, server->stack, &server_data.request_count, 1, 1000));
	server_transaction1 = server_data.server_transaction;

	/* send a second request with different channel bank identifier*/
	req = belle_sip_request_create(
	    belle_sip_uri_parse("sip:example.org"), "OPTIONS", belle_sip_provider_create_call_id(client->provider),
	    belle_sip_header_cseq_create(20, "OPTIONS"),
	    belle_sip_header_from_create2("sip:bob@example.org", BELLE_SIP_RANDOM_TAG),
	    belle_sip_header_to_create2("sip:alice@example.org", NULL), belle_sip_header_via_new(), 70);
	belle_sip_message_set_channel_bank_identifier(BELLE_SIP_MESSAGE(req), "yyy");

	client_transaction2 = belle_sip_provider_create_client_transaction(client->provider, req);
	belle_sip_client_transaction_send_request_to(client_transaction2, server->lp->listening_uri);
	belle_sip_object_ref(client_transaction2);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(client->stack, server->stack, &server_data.request_count, 2, 1000));
	server_transaction2 = server_data.server_transaction;

	/* Assert that all channels used so first and second transactions were different */
	BC_ASSERT_PTR_NOT_NULL(server_transaction1->base.channel);
	BC_ASSERT_PTR_NOT_NULL(server_transaction2->base.channel);
	BC_ASSERT_PTR_NOT_NULL(client_transaction1->base.channel);
	BC_ASSERT_PTR_NOT_NULL(client_transaction2->base.channel);

	BC_ASSERT_TRUE(server_transaction1->base.channel != server_transaction2->base.channel);
	BC_ASSERT_TRUE(client_transaction1->base.channel != client_transaction2->base.channel);

	client_port1 = client_transaction1->base.channel->local_port;
	client_port2 = client_transaction2->base.channel->local_port;

	BC_ASSERT_TRUE(client_port1 != client_port2);

	belle_sip_object_unref(client_transaction1);
	belle_sip_object_unref(client_transaction2);
	belle_sip_object_unref(server_transaction1);
	belle_sip_object_unref(server_transaction2);

	belle_sip_tester_wait_for(client->stack, server->stack, NULL, 0, 1000);

	/* now the server sends a request back to the client, through the first channel */
	req = belle_sip_request_create(
	    belle_sip_uri_parse("sip:bob@example.org"), "OPTIONS", belle_sip_provider_create_call_id(client->provider),
	    belle_sip_header_cseq_create(20, "OPTIONS"),
	    belle_sip_header_from_create2("sip:alice@example.org", BELLE_SIP_RANDOM_TAG),
	    belle_sip_header_to_create2("sip:bob@example.org", NULL), belle_sip_header_via_new(), 70);
	/* no channel bank identifier is given. */
	dest_uri = belle_sip_uri_parse("sip:127.0.0.1");
	belle_sip_uri_set_port(dest_uri, client_port1);
	client_transaction1 = belle_sip_provider_create_client_transaction(server->provider, req);
	belle_sip_client_transaction_send_request_to(client_transaction1, dest_uri);
	belle_sip_object_ref(client_transaction1);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(client->stack, server->stack, &client_data.request_count, 1, 1000));
	server_transaction1 = client_data.server_transaction;

	/* server sends a request back to the client, through the second channel */
	req = belle_sip_request_create(
	    belle_sip_uri_parse("sip:bob@example.org"), "OPTIONS", belle_sip_provider_create_call_id(client->provider),
	    belle_sip_header_cseq_create(20, "OPTIONS"),
	    belle_sip_header_from_create2("sip:alice@example.org", BELLE_SIP_RANDOM_TAG),
	    belle_sip_header_to_create2("sip:bob@example.org", NULL), belle_sip_header_via_new(), 70);
	/* no channel bank identifier is given. */
	dest_uri = belle_sip_uri_parse("sip:127.0.0.1");
	belle_sip_uri_set_port(dest_uri, client_port2);
	client_transaction2 = belle_sip_provider_create_client_transaction(server->provider, req);
	belle_sip_client_transaction_send_request_to(client_transaction2, dest_uri);
	belle_sip_object_ref(client_transaction2);
	BC_ASSERT_TRUE(belle_sip_tester_wait_for(client->stack, server->stack, &client_data.request_count, 2, 1000));
	server_transaction2 = client_data.server_transaction;

	/* Assert that all channels used so first and second transactions were different */
	BC_ASSERT_PTR_NOT_NULL(server_transaction1->base.channel);
	BC_ASSERT_PTR_NOT_NULL(server_transaction2->base.channel);
	BC_ASSERT_PTR_NOT_NULL(client_transaction1->base.channel);
	BC_ASSERT_PTR_NOT_NULL(client_transaction2->base.channel);

	BC_ASSERT_TRUE(server_transaction1->base.channel != server_transaction2->base.channel);
	BC_ASSERT_TRUE(client_transaction1->base.channel != client_transaction2->base.channel);

	belle_sip_object_unref(client_transaction1);
	belle_sip_object_unref(client_transaction2);
	belle_sip_object_unref(server_transaction1);
	belle_sip_object_unref(server_transaction2);

	belle_sip_message("Done.");

	belle_sip_tester_destroy_endpoint(client);
	belle_sip_tester_destroy_endpoint(server);
}

static test_t refresher_tests[] = {
    TEST_NO_TAG("REGISTER Expires header", register_expires_header),
    TEST_NO_TAG("REGISTER Expires in Contact", register_expires_in_contact),
    TEST_NO_TAG("REGISTER Expires header digest", register_expires_header_digest),
    TEST_NO_TAG("REGISTER Expires in Contact digest auth", register_expires_in_contact_header_digest_auth),
    TEST_NO_TAG("REGISTER Expires in Contact digest auth SHA-256",
                register_expires_in_contact_header_digest_auth_sha256),
    TEST_NO_TAG("REGISTER Expires in Contact digest auth MD_SHA", register_expires_in_contact_header_digest_md_sha),
    TEST_NO_TAG("REGISTER Expires in Contact digest auth MD_SHA ha1",
                register_expires_in_contact_header_digest_md_sha_ha1),
    TEST_NO_TAG("REGISTER Expires in Contact digest auth MD_SHA-256",
                register_expires_in_contact_header_digest_md_sha256),
    TEST_NO_TAG("REGISTER Expires in Contact digest auth MD_SHA-256 ha1",
                register_expires_in_contact_header_digest_md_sha256_ha1),
    TEST_NO_TAG("REGISTER with failure", register_with_failure),
    TEST_NO_TAG("REGISTER failed because client does not allow MD5", register_with_failure_because_of_md5),
    TEST_NO_TAG("REGISTER failed because client requires qop=auth", register_with_failure_because_of_no_qop),
    TEST_NO_TAG("REGISTER successful with SHA-256 when MD5 is disabled.", refresher_sha256_with_md5_disabled),
    TEST_NO_TAG("REGISTER with early refresher", register_early_refresher),
    TEST_NO_TAG("REGISTER with retry after", register_retry_after),
    TEST_NO_TAG("SUBSCRIBE", subscribe_test),
    TEST_NO_TAG("SUBSCRIBE of list", subscribe_list_test),
    TEST_NO_TAG("PUBLISH", simple_publish),
    TEST_NO_TAG("PUBLISH with early refresher", simple_publish_with_early_refresher),
    TEST_NO_TAG("REGISTER with unrecognizable Contact", register_with_unrecognizable_contact),
    TEST_NO_TAG("REGISTER UDP from ipv6 to ipv4", register_test_ipv6_to_ipv4),
    TEST_NO_TAG("REGISTER UDP from ipv4 to ipv6", register_test_ipv4_to_ipv6),
    TEST_NO_TAG("REGISTER UDP from ipv6 to ipv6 with ipv4", register_test_ipv6_to_ipv6_with_ipv4),
    TEST_NO_TAG("REGISTER UDP from ipv6 to ipv6 with ipv6", register_test_ipv6_to_ipv6_with_ipv6),
    TEST_NO_TAG("REGISTER TCP from ipv6 to ipv4", register_tcp_test_ipv6_to_ipv4),
    TEST_NO_TAG("REGISTER TCP from ipv4 to ipv6", register_tcp_test_ipv4_to_ipv6),
    TEST_NO_TAG("REGISTER TCP from ipv6 to ipv6 with ipv4", register_tcp_test_ipv6_to_ipv6_with_ipv4),
    TEST_NO_TAG("REGISTER TCP from ipv6 to ipv6 with ipv6", register_tcp_test_ipv6_to_ipv6_with_ipv6),
    TEST_NO_TAG("REGISTER UDP from random port using AF_INET", register_udp_test_ipv4_random_port),
    TEST_NO_TAG("REGISTER UDP from random port using AF_INET6", register_udp_test_ipv6_random_port),
    TEST_NO_TAG("REGISTER TCP from random port using AF_INET", register_tcp_test_ipv4_random_port),
    TEST_NO_TAG("REGISTER TCP from random port using AF_INET6", register_tcp_test_ipv6_random_port),
    TEST_NO_TAG("REGISTER AND PUBLISH", register_and_publish),
    TEST_NO_TAG("REGISTER, digest with next nonce", register_digest_with_next_nonce),
    TEST_NO_TAG("REGISTER, digest auth with next nonce", register_digest_auth_with_next_nonce),
    TEST_NO_TAG("REGISTER, digest auth with bad next nonce", register_digest_auth_with_bad_next_nonce),
    TEST_NO_TAG("REGISTER with RFC5626 ping pong ok", register_with_pingpong_pong_ok),
    TEST_NO_TAG("REGISTER with RFC5626 ping pong nok", register_with_pingpong_pong_nok),
    TEST_NO_TAG("REGISTER with RFC5626 ping pong and without verification",
                register_with_pingpong_without_pingpong_verification),
    TEST_NO_TAG("REGISTER with RFC5626 ping pong not supported server-side", register_with_pingpong_not_supported),
    TEST_NO_TAG("REGISTER with RFC5626 ping pong erroneously supported server-side",
                register_with_pingpong_not_supported_2),
    TEST_NO_TAG("Transactions using different channel banks", channel_bank_test)

};

test_suite_t refresher_test_suite = {"Refresher",
                                     NULL,
                                     NULL,
                                     belle_sip_tester_before_each,
                                     belle_sip_tester_after_each,
                                     sizeof(refresher_tests) / sizeof(refresher_tests[0]),
                                     refresher_tests,
                                     0,
                                     0};
