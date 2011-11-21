#include "uwsgi.h"

/*

	subscription subsystem

	each subscription slot is as an auto-optmizing linked list. Originally it was a uwsgi_dict (now removed from uWSGI) but this
	would have not be able to support regexp as keys.

	each slot has another circular linked list containing the nodes names

	the structure and system is very similar to uwsgi_dyn_dict already used by the mime type parser

	This system is not mean to run on shared memory. If you have multiple processes for the same app, you have to create
	a new subscriptions slot list.

	To avoid removal of already using nodes, a reference count system has been implemented

*/

extern struct uwsgi_server uwsgi;

struct uwsgi_subscribe_slot *uwsgi_get_subscribe_slot(struct uwsgi_subscribe_slot **slot, char *key, uint16_t keylen, int regexp) {

	struct uwsgi_subscribe_slot *current_slot = *slot;

	if (keylen > 0xff) return NULL;

	while(current_slot) {
#ifdef UWSGI_PCRE
		if (regexp) {
			if (uwsgi_regexp_match(current_slot->pattern, current_slot->pattern_extra, key, keylen) >= 0) {
				return current_slot;
			}
		}
		else {
#endif
			if (!uwsgi_strncmp(key, keylen, current_slot->key, current_slot->keylen)) {
                		// auto optimization
                        	if (current_slot->prev) {
                                        if (current_slot->hits > current_slot->prev->hits) {
                                                struct uwsgi_subscribe_slot *slot_parent = current_slot->prev->prev, *slot_prev = current_slot->prev;
                                                if (slot_parent) {
                                                       slot_parent->next = current_slot;
                                                }
						else {
							*slot = current_slot;
						}

                                                slot_prev->prev = current_slot;
                                                slot_prev->next = current_slot->next;

                                                current_slot->next = slot_prev;
						current_slot->prev = slot_parent;

                        		}
                        	}
				return current_slot;
			}
#ifdef UWSGI_PCRE
		}
#endif
		current_slot = current_slot->next;
	}

        return NULL;
}

struct uwsgi_subscribe_node *uwsgi_get_subscribe_node(struct uwsgi_subscribe_slot **slot, char *key, uint16_t keylen, int regexp) {

	if (keylen > 0xff) return NULL;

	struct uwsgi_subscribe_slot *current_slot = uwsgi_get_subscribe_slot(slot, key, keylen, regexp);
	uint64_t rr_pos = 0;

	if (current_slot) {
		// node found, move up in the list increasing hits
		current_slot->hits++;
		time_t current = time(NULL);
		struct uwsgi_subscribe_node *node = current_slot->nodes;
		while(current_slot && node) {
			// is the node alive ?
			if (current - node->last_check > uwsgi.subscription_tolerance) {
				node->death_mark = 1;
			}
			if (node->death_mark && node->reference == 0) {
				// remove the node and move to next
				struct uwsgi_subscribe_node *dead_node = node;
				node = node->next;
				// if the slot has been removed, return NULL;
				if (uwsgi_remove_subscribe_node(slot, dead_node) == 1) {
					return NULL;
				}
				continue;
			}
			if (rr_pos == current_slot->rr) {
				current_slot->rr++;
				node->reference++;
				return node;
			}
			node = node->next;
			rr_pos++;
		}
		current_slot->rr = 0;
		if (current_slot->nodes) {
			current_slot->nodes->reference++;
		}
		return current_slot->nodes;
	}

	return NULL;
}

struct uwsgi_subscribe_node *uwsgi_get_subscribe_node_by_name(struct uwsgi_subscribe_slot **slot, char *key, uint16_t keylen, char *val, uint16_t vallen, int regexp) {

	if (keylen > 0xff) return NULL;
	struct uwsgi_subscribe_slot *current_slot = uwsgi_get_subscribe_slot(slot, key, keylen, regexp);
	if (current_slot) {
		struct uwsgi_subscribe_node *node = current_slot->nodes;
		while(node) {
			if (!uwsgi_strncmp(val, vallen, node->name, node->len)) {
				return node;
			}			
			node = node->next;
		}
	}

	return NULL;
}

int uwsgi_remove_subscribe_node(struct uwsgi_subscribe_slot **slot, struct uwsgi_subscribe_node *node) {

	int ret = 0;

	struct uwsgi_subscribe_node *a_node;
	struct uwsgi_subscribe_slot *node_slot = node->slot;
	struct uwsgi_subscribe_slot *prev_slot = node_slot->prev;
	struct uwsgi_subscribe_slot *next_slot = node_slot->next;

	// over-engineering to avoid race conditions
	node->len = 0;

	if (node == node_slot->nodes) {
		node_slot->nodes = node->next;
	}
	else {
		a_node = node_slot->nodes;
		while(a_node) {
			if (a_node->next == node) {
				a_node->next = node->next;
				break;
			}
			a_node = a_node->next;
		}
	}

	free(node);
	// no more nodes, remove the slot too
	if (node_slot->nodes == NULL) {
			
		if (prev_slot) {	
			prev_slot->next = next_slot;	
		}
		if (next_slot) {
			next_slot->prev = prev_slot;
		}

#ifdef UWSGI_PCRE
		if (node_slot->pattern) {
			pcre_free(node_slot->pattern);
		}
		if (node_slot->pattern_extra) {
			pcre_free(node_slot->pattern_extra);
		}
#endif

		ret = 1;
		free(node_slot);
		// am i the only slot ?
		if (!prev_slot && !next_slot) {
			*slot = NULL;
		}
	}

	return ret;
}

struct uwsgi_subscribe_node *uwsgi_add_subscribe_node(struct uwsgi_subscribe_slot **slot, struct uwsgi_subscribe_req *usr, int regexp) {

	struct uwsgi_subscribe_slot *current_slot = uwsgi_get_subscribe_slot(slot, usr->key, usr->keylen, 0), *old_slot = NULL, *a_slot;
	struct uwsgi_subscribe_node *node, *old_node = NULL;

	if (usr->address_len > 0xff) return NULL;

        if (current_slot) {
		node = current_slot->nodes;
		while(node) {
                        if (!uwsgi_strncmp(node->name, node->len, usr->address, usr->address_len)) {
				// remove death mark
				node->death_mark = 0;
                                node->last_check = time(NULL);
                                return node;
                        }
			old_node = node;
			node = node->next;
                }

		node = uwsgi_malloc(sizeof(struct uwsgi_subscribe_node));
		node->len = usr->address_len;
		node->modifier1 = usr->modifier1;
		node->modifier2 = usr->modifier2;
		node->reference = 0;
		node->death_mark = 0;
		node->last_check = time(NULL);
		node->slot = current_slot;
                memcpy(node->name, usr->address, usr->address_len);
		if (old_node) {
			old_node->next = node;
		}
		node->next = NULL;
                uwsgi_log("[uwsgi-subscription] %.*s => new node: %.*s\n", usr->keylen, usr->key, usr->address_len, usr->address);
                return node;
        }
        else {

		current_slot = uwsgi_malloc(sizeof(struct uwsgi_subscribe_slot));
		current_slot->keylen = usr->keylen;
		memcpy(current_slot->key, usr->key, usr->keylen);
		current_slot->key[usr->keylen] = 0;
		current_slot->hits = 0;
		current_slot->rr = 0;

#ifdef UWSGI_PCRE
		current_slot->pattern = NULL;
		current_slot->pattern_extra = NULL;
		if (regexp) {
			if (uwsgi_regexp_build(current_slot->key, &current_slot->pattern, &current_slot->pattern_extra)) {
				free(current_slot);
				return NULL;
			}
		}
#endif

		current_slot->nodes = uwsgi_malloc(sizeof(struct uwsgi_subscribe_node));
		current_slot->nodes->slot = current_slot;
		current_slot->nodes->len = usr->address_len;
		current_slot->nodes->reference = 0;
		current_slot->nodes->death_mark = 0;
		current_slot->nodes->modifier1 = usr->modifier1;
		current_slot->nodes->modifier2 = usr->modifier2;
		memcpy(current_slot->nodes->name, usr->address, usr->address_len);
		current_slot->nodes->last_check = time(NULL);

		current_slot->nodes->next = NULL;

#ifdef UWSGI_PCRE
		// if key is a regexp, order it by keylen
		if (regexp) {
			old_slot = NULL;
			a_slot = *slot;
			while(a_slot) {
				if (a_slot->keylen > current_slot->keylen) {
					old_slot = a_slot;
					break;
				}	
				a_slot = a_slot->next;
			}

			if (old_slot) {
				current_slot->prev = old_slot->prev;
				old_slot->prev = current_slot;
				if (current_slot->prev) {
					old_slot->prev->next = current_slot;
				}
	
				current_slot->next = old_slot;
			}
			else {
				a_slot = *slot;
                        	while(a_slot) {
                                	old_slot = a_slot;
                                	a_slot = a_slot->next;
                        	}


                        	if (old_slot) {
                                	old_slot->next = current_slot;
                        	}

                        	current_slot->prev = old_slot;
                        	current_slot->next = NULL;
			}
		}
		else {
#endif
			a_slot = *slot;
			while(a_slot) {
				old_slot = a_slot;
				a_slot = a_slot->next;
			}


			if (old_slot) {
				old_slot->next = current_slot;
			}

			current_slot->prev = old_slot;
			current_slot->next = NULL;

#ifdef UWSGI_PCRE
		}
#endif

		if (!*slot || current_slot->prev == NULL) {
			*slot = current_slot;
		}

		uwsgi_log("[uwsgi-subscription] new pool: %.*s\n", usr->keylen, usr->key);
		uwsgi_log("[uwsgi-subscription] %.*s => new node: %.*s\n", usr->keylen, usr->key, usr->address_len, usr->address);
                return current_slot->nodes;
        }

}


void uwsgi_send_subscription(char *udp_address, char *key, size_t keysize, char *modifier1, size_t modifier1_len, uint8_t cmd) {

	size_t ssb_size = 4 + (2 + 3) + (2 + keysize) + (2 + 7) + (2 + strlen(uwsgi.sockets->name));

	if (modifier1) {
		ssb_size += (2 + 9) + (2 + modifier1_len);
	}

        char *subscrbuf = uwsgi_malloc(ssb_size);
	// leave space for uwsgi header
        char *ssb = subscrbuf+4;

	// key = "domain"
        uint16_t ustrlen = 3;
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, "key", ustrlen);
        ssb+=ustrlen;

        ustrlen = keysize;
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, key, ustrlen);
        ssb+=ustrlen;

	// address = "first uwsgi socket"
        ustrlen = 7;
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, "address", ustrlen);
        ssb+=ustrlen;

        ustrlen = strlen(uwsgi.sockets->name);
        *ssb++ = (uint8_t) (ustrlen  & 0xff);
        *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
        memcpy(ssb, uwsgi.sockets->name, ustrlen);
        ssb+=ustrlen;

	// modifier1 = "modifier1"
        if (modifier1) {
                ustrlen = 9;
                *ssb++ = (uint8_t) (ustrlen  & 0xff);
                *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
                memcpy(ssb, "modifier1", ustrlen);
                ssb+=ustrlen;

                ustrlen = modifier1_len;
                *ssb++ = (uint8_t) (ustrlen  & 0xff);
                *ssb++ = (uint8_t) ((ustrlen >>8) & 0xff);
                memcpy(ssb, modifier1, ustrlen);
                ssb+=ustrlen;
        }

        send_udp_message(224, cmd, udp_address, subscrbuf, ssb_size-4);
}
