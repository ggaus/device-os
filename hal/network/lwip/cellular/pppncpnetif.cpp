/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "logging.h"
LOG_SOURCE_CATEGORY("net.pppncp")

#include "pppncpnetif.h"
#include <lwip/opt.h>
#include <lwip/mem.h>
#include <lwip/pbuf.h>
#include <lwip/ethip6.h>
#include <lwip/etharp.h>
#include "delay_hal.h"
#include "timer_hal.h"
#include "gpio_hal.h"
#include "service_debug.h"
#include <netif/ethernet.h>
#include <lwip/netifapi.h>
#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <algorithm>
#include "lwiplock.h"
#include "interrupts_hal.h"
#include "ringbuffer.h"
#include "network/ncp/cellular/cellular_ncp_client.h"
#include "concurrent_hal.h"
#include "platform_config.h"
#include "memp_hook.h"

using namespace particle::net;

namespace {

enum class NetifEvent {
    None = 0,
    Up = 1,
    Down = 2,
    Exit = 3,
    PowerOff = 4,
    PowerOn = 5
};

// 5 minutes
const unsigned PPP_CONNECT_TIMEOUT = 5 * 60 * 1000;

// FIXME: unnecessarily large but helps avoid deadlocks/missing events between LwIP and NCP contexts
const auto NCP_NETIF_EVENT_QUEUE_SIZE = 32;

} // anonymous


PppNcpNetif::PppNcpNetif()
        : BaseNetif(),
          exit_(false),
          expectedNetifState_(NETIF_STATE_OFF) {

    LOG(INFO, "Creating PppNcpNetif LwIP interface");

    SPARK_ASSERT(os_queue_create(&queue_, sizeof(NetifEvent), NCP_NETIF_EVENT_QUEUE_SIZE, nullptr) == 0);

    client_.setNotifyCallback(pppEventHandlerCb, this);
    client_.start();
}

PppNcpNetif::~PppNcpNetif() {
    exit_ = true;
    if (thread_ && queue_) {
        auto ex = NetifEvent::Exit;
        os_queue_put(queue_, &ex, 1000, nullptr);
        os_thread_join(thread_);
        os_queue_destroy(queue_, nullptr);
    }
}

void PppNcpNetif::init() {
    registerHandlers();
    SPARK_ASSERT(lwip_memp_event_handler_add(mempEventHandler, MEMP_PBUF_POOL, this) == 0);
    SPARK_ASSERT(os_thread_create(&thread_, "pppncp", OS_THREAD_PRIORITY_NETWORK, &PppNcpNetif::loop, this, OS_THREAD_STACK_SIZE_DEFAULT) == 0);
}

void PppNcpNetif::setCellularManager(CellularNetworkManager* celMan) {
    celMan_ = celMan;
}

if_t PppNcpNetif::interface() {
    return (if_t)client_.getIf();
}

void PppNcpNetif::loop(void* arg) {
    PppNcpNetif* self = static_cast<PppNcpNetif*>(arg);
    unsigned int timeout = 100;
    while(!self->exit_) {
        NetifEvent ev;
        const int r = os_queue_take(self->queue_, &ev, timeout, nullptr);
        self->celMan_->ncpClient()->enable(); // Make sure the client is enabled
        if (!r) {
            // Event
            LOG(TRACE, "PPP netif event from queue: %d", ev);
            switch (ev) {
                case NetifEvent::Up: {
                    self->expectedNetifState_ = NETIF_STATE_UP;
                    break;
                }
                case NetifEvent::Down: {
                    if (self->celMan_->ncpClient()->ncpState() == NcpState::ON) {
                        self->expectedNetifState_ = NETIF_STATE_ON_DOWN;
                    } else {
                        self->expectedNetifState_ = NETIF_STATE_OFF;
                    }
                    break;
                }
                case NetifEvent::PowerOff: {
                    self->expectedNetifState_ = NETIF_STATE_OFF;
                    break;
                }
                case NetifEvent::PowerOn: {
                    if (self->expectedNetifState_ == NETIF_STATE_OFF) {
                        self->expectedNetifState_ = NETIF_STATE_ON_DOWN;
                    }
                    break;
                }
            }
        }

        if (self->expectedNetifState_ != NETIF_STATE_OFF && self->celMan_->ncpClient()->ncpState() != NcpState::ON) {
            auto r = self->celMan_->ncpClient()->on();
            if (r != SYSTEM_ERROR_NONE && r != SYSTEM_ERROR_ALREADY_EXISTS) {
                LOG(ERROR, "Failed to initialize cellular NCP client: %d", r);
            }
        }
        if (self->expectedNetifState_ != NETIF_STATE_UP && self->celMan_->ncpClient()->connectionState() != NcpConnectionState::DISCONNECTED) {
            self->downImpl();
        }
        if (self->celMan_->ncpClient()->connectionState() == NcpConnectionState::DISCONNECTED) {
            if_power_state_t pwrState = IF_POWER_STATE_NONE;
            self->getPowerState(&pwrState);
            if (self->expectedNetifState_ == NETIF_STATE_UP && self->celMan_->ncpClient()->ncpState() == NcpState::ON) {
                self->upImpl();
            } else if (self->expectedNetifState_ == NETIF_STATE_OFF && pwrState == IF_POWER_STATE_UP) {
                auto r = self->celMan_->ncpClient()->off();
                if (r != SYSTEM_ERROR_NONE) {
                    LOG(ERROR, "Failed to turn off NCP client: %d", r);
                }
            }
        }

        self->celMan_->ncpClient()->processEvents();

        if (self->expectedNetifState_ == NETIF_STATE_UP && self->celMan_->ncpClient()->connectionState() == NcpConnectionState::CONNECTED) {
            auto start = self->connectStart_;
            if (start != 0 && HAL_Timer_Get_Milli_Seconds() - start >= PPP_CONNECT_TIMEOUT) {
                LOG(ERROR, "Failed to bring up PPP session after %lu seconds", PPP_CONNECT_TIMEOUT / 1000);
                self->client_.disconnect();
                self->celMan_->ncpClient()->disable();
            }
        }
    }

    self->downImpl();
    self->celMan_->ncpClient()->off();

    os_thread_exit(nullptr);
}

int PppNcpNetif::up() {
    NetifEvent ev = NetifEvent::Up;
    return os_queue_put(queue_, &ev, 0, nullptr);
}

int PppNcpNetif::down() {
    const auto client = celMan_->ncpClient();
    /* Note: It's possible that we have powered on the modem but the NCP client initialization is still in progress.
     * For example, invokes Cellular.connect() followed by Cellular.disconnect().
     * If we interrupted the operation, we will probably fail to turn off the modem in enable() that is invoked in the PPP thread.
     * It would result in a hard reset of the modem, which will take longer to turn off the modem. Simply let the NCP client
     * finalize the initialization procedure and make it reside in ON state. */
    if (client->connectionState() != NcpConnectionState::CONNECTED && client->ncpState() == NcpState::ON) {
        // Disable the client to interrupt its current operation
        client->disable();
    }
    NetifEvent ev = NetifEvent::Down;
    int r = os_queue_put(queue_, &ev, 0, nullptr);
    if (r) {
        if (client->ncpState() == NcpState::ON) {
            client->disable();
        }
    }
    return r;
}

int PppNcpNetif::powerUp() {
    NetifEvent ev = NetifEvent::PowerOn;
    return os_queue_put(queue_, &ev, CONCURRENT_WAIT_FOREVER, nullptr);
}

int PppNcpNetif::powerDown() {
    NetifEvent ev = NetifEvent::PowerOff;
    return os_queue_put(queue_, &ev, CONCURRENT_WAIT_FOREVER, nullptr);
}

int PppNcpNetif::getPowerState(if_power_state_t* state) const {
    auto s = celMan_->ncpClient()->ncpPowerState();
    if (s == NcpPowerState::ON) {
        *state = IF_POWER_STATE_UP;
    } else if (s == NcpPowerState::OFF) {
        *state = IF_POWER_STATE_DOWN;
    } else if (s == NcpPowerState::TRANSIENT_ON) {
        *state = IF_POWER_STATE_POWERING_UP;
    } else if (s == NcpPowerState::TRANSIENT_OFF) {
        *state = IF_POWER_STATE_POWERING_DOWN;
    } else {
        *state = IF_POWER_STATE_NONE;
    }
    return SYSTEM_ERROR_NONE;
}

int PppNcpNetif::upImpl() {
    client_.setOutputCallback([](const uint8_t* data, size_t size, void* ctx) -> int {
        auto c = (CellularNcpClient*)ctx;
        int r = c->dataChannelWrite(0, data, size);
        if (!r) {
            return size;
        }
        return r;
    }, celMan_->ncpClient());
    client_.setEnterDataModeCallback([](void* ctx) -> int {
        auto c = (CellularNcpClient*)ctx;
        return c->enterDataMode();
    }, celMan_->ncpClient());
    // Initialize PPP client
    client_.connect();

    auto r = celMan_->connect();
    if (r) {
        LOG(TRACE, "Failed to connect to cellular network: %d", r);
        // Make sure to re-enable NCP client
        celMan_->ncpClient()->enable();
        // And turn it off just in case
        celMan_->ncpClient()->off();
    }
    return r;
}

int PppNcpNetif::downImpl() {
    int r = SYSTEM_ERROR_NONE;
    client_.notifyEvent(ppp::Client::EVENT_LOWER_DOWN);
    client_.disconnect();

    if (celMan_->ncpClient()->connectionState() != NcpConnectionState::DISCONNECTED) {
        r = celMan_->ncpClient()->disconnect();
        if (r != SYSTEM_ERROR_NONE) {
            // Make sure to re-enable NCP client
            celMan_->ncpClient()->enable();
        }
    }
    return r;
}

void PppNcpNetif::ifEventHandler(const if_event* ev) {
    if (ev->ev_type == IF_EVENT_STATE) {
        int r = 0;
        if (ev->ev_if_state->state) {
            r = up();
        } else {
            r = down();
        }
        if (r) {
            LOG(ERROR, "Failed to post iface event");
        }
    }
}

void PppNcpNetif::netifEventHandler(netif_nsc_reason_t reason, const netif_ext_callback_args_t* args) {
    /* Nothing to do here */
}

void PppNcpNetif::pppEventHandlerCb(particle::net::ppp::Client* c, uint64_t ev, void* ctx) {
    auto self = (PppNcpNetif*)ctx;
    self->pppEventHandler(ev);
}

void PppNcpNetif::pppEventHandler(uint64_t ev) {
    if (ev == particle::net::ppp::Client::EVENT_UP) {
        unsigned mtu = client_.getIf()->mtu;
        LOG(TRACE, "Negotiated MTU: %u", mtu);
        // Reset
        connectStart_ = 0;
    } else if (ev == particle::net::ppp::Client::EVENT_CONNECTING) {
        if (connectStart_ == 0) {
            connectStart_ = HAL_Timer_Get_Milli_Seconds();
        }
    }
}

void PppNcpNetif::ncpEventHandlerCb(const NcpEvent& ev, void* ctx) {
    LOG(TRACE, "NCP event %d", (int)ev.type);
    auto self = (PppNcpNetif*)ctx;
    if (ev.type == NcpEvent::CONNECTION_STATE_CHANGED) {
        const auto& cev = static_cast<const NcpConnectionStateChangedEvent&>(ev);
        LOG(TRACE, "State changed event: %d", (int)cev.state);
        switch (cev.state) {
            case NcpConnectionState::DISCONNECTED:
            case NcpConnectionState::CONNECTING: {
                self->client_.notifyEvent(ppp::Client::EVENT_LOWER_DOWN);
                self->connectStart_ = 0;
                break;
            }
            case NcpConnectionState::CONNECTED: {
                self->connectStart_ = 0;
                self->client_.notifyEvent(ppp::Client::EVENT_LOWER_UP);
            }
        }
    } else if (ev.type == CellularNcpEvent::AUTH) {
        const auto& cev = static_cast<const CellularNcpAuthEvent&>(ev);
        LOG(TRACE, "New auth info");
        self->client_.setAuth(cev.user, cev.password);
    } else if (ev.type == NcpEvent::POWER_STATE_CHANGED) {
        const auto& cev = static_cast<const NcpPowerStateChangedEvent&>(ev);
        if (cev.state != NcpPowerState::UNKNOWN) {
            if_event evt = {};
            struct if_event_power_state ev_if_power_state = {};
            evt.ev_len = sizeof(if_event);
            evt.ev_type = IF_EVENT_POWER_STATE;
            evt.ev_power_state = &ev_if_power_state;
            if (cev.state == NcpPowerState::ON) {
                evt.ev_power_state->state = IF_POWER_STATE_UP;
                LOG(TRACE, "NCP power state changed: IF_POWER_STATE_UP");
            } else if (cev.state == NcpPowerState::OFF) {
                evt.ev_power_state->state = IF_POWER_STATE_DOWN;
                LOG(TRACE, "NCP power state changed: IF_POWER_STATE_DOWN");
            } else if (cev.state == NcpPowerState::TRANSIENT_ON) {
                evt.ev_power_state->state = IF_POWER_STATE_POWERING_UP;
                LOG(TRACE, "NCP power state changed: IF_POWER_STATE_POWERING_UP");
            } else {
                evt.ev_power_state->state = IF_POWER_STATE_POWERING_DOWN;
                LOG(TRACE, "NCP power state changed: IF_POWER_STATE_POWERING_DOWN");
            }
            if_notify_event(self->interface(), &evt, nullptr);
        } else {
            LOG(ERROR, "NCP power state unknown");
        }
    }
}

int PppNcpNetif::ncpDataHandlerCb(int id, const uint8_t* data, size_t size, void* ctx) {
    PppNcpNetif* self = static_cast<PppNcpNetif*>(ctx);
    int r = self->client_.input(data, size);
    if (r == SYSTEM_ERROR_NO_MEMORY) {
        self->celMan_->ncpClient()->dataChannelFlowControl(true);
    }
    return r;
}

void PppNcpNetif::mempEventHandler(memp_t type, unsigned available, unsigned size, void* ctx) {
    PppNcpNetif* self = static_cast<PppNcpNetif*>(ctx);
    if (available > HAL_PLATFORM_PACKET_BUFFER_FLOW_CONTROL_THRESHOLD) {
        self->celMan_->ncpClient()->dataChannelFlowControl(false);
    }
}
