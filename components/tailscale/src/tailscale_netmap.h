/*
 * Tailscale network map — MapResponse JSON parser and WireGuard peer manager.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "tailscale_derp.h"

/**
 * @brief Apply a JSON MapResponse string to the WireGuard data plane.
 *
 * Parses the MapResponse received from the Tailscale control server and
 * synchronises the WireGuard peer table:
 *   - Adds peers that are new in this map.
 *   - Removes peers that are no longer present.
 *   - Updates endpoints of existing peers when they change.
 *
 * Also stores the DERPMap into the DERP client for relay selection.
 *
 * @param json_str  NULL-terminated JSON string (MapResponse).
 * @param json_len  Length of json_str (excluding NUL).
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if the JSON is malformed.
 */
esp_err_t ts_netmap_apply(const char *json_str, size_t json_len);

/**
 * @brief Retrieve the DERP home server node selected from the last DERPMap.
 *
 * @param node_out  Output: filled with the best DERP node on success.
 * @return true if a DERP home has been set, false otherwise.
 */
bool ts_netmap_get_derp_home(ts_derp_node_t *node_out);

/**
 * @brief Return the Tailscale IP address of this node.
 *
 * Valid only after at least one successful ts_netmap_apply() call that
 * contained a "Node" field with an "Addresses" entry.
 *
 * @param ip_str    Buffer to receive the IP string (e.g. "100.x.y.z").
 * @param ip_str_len  Buffer length; 20 bytes is sufficient.
 */
void ts_netmap_get_self_ip(char *ip_str, size_t ip_str_len);
