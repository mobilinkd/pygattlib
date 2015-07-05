// -*- mode: c++; coding: utf-8 -*-

// Copyright (C) 2014, Oscar Acena <oscaracena@gmail.com>
// This software is under the terms of GPLv3 or later.

#include <boost/thread/thread.hpp>
#include <boost/python/dict.hpp>
#include <sys/ioctl.h>
#include <iostream>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "gattlib.h"

class PyThreadsGuard {
public:
    PyThreadsGuard() : _save(NULL) {
        _save = PyEval_SaveThread();
    };

    ~PyThreadsGuard() {
        PyEval_RestoreThread(_save);
    };

private:
    PyThreadState* _save;
};


IOService::IOService(bool run) {
    if (run)
        start();
}

void
IOService::start() {
    if (!PyEval_ThreadsInitialized()) {
        PyEval_InitThreads();
    }

    boost::thread iothread(*this);
}

void
IOService::operator()() {
    GMainLoop *event_loop = g_main_loop_new(NULL, FALSE);

    g_main_loop_run(event_loop);
    g_main_loop_unref(event_loop);
}

static volatile IOService _instance(true);

GATTResponse::GATTResponse() :
    _status(0) {
}

void
GATTResponse::on_response(const std::string data) {
    _data.append(data);
}

void
GATTResponse::on_response(boost::python::object data) {
    _data.append(data);
}

void
GATTResponse::notify(uint8_t status) {
    _status = status;
    _event.set();
}

bool
GATTResponse::wait(uint16_t timeout) {
    if (not _event.wait(timeout))
        return false;

    if (_status != 0) {
        std::string msg = "Characteristic value/descriptor operation failed: ";
        msg += att_ecode2str(_status);
        throw std::runtime_error(msg);
    }

    return true;
}

boost::python::list
GATTResponse::received() {
    return _data;
}

GATTRequester::GATTRequester(std::string address, bool do_connect,
        std::string device) :
    _state(STATE_DISCONNECTED),
    _device(device),
    _address(address),
    _hci_socket(-1),
    _channel(NULL),
    _attrib(NULL) {

    int dev_id = hci_devid(_device.c_str());
    if (dev_id < 0)
        throw std::runtime_error("Invalid device!");

    _hci_socket = hci_open_dev(dev_id);
    if (_hci_socket < 0) {
        std::string msg = std::string("Could not open HCI device: ") +
            std::string(strerror(errno));
        throw std::runtime_error(msg);
    }

    if (do_connect)
        connect();
}

GATTRequester::~GATTRequester() {
    if (_channel != NULL) {
        g_io_channel_shutdown(_channel, TRUE, NULL);
        g_io_channel_unref(_channel);
    }

    if (_hci_socket > -1)
        hci_close_dev(_hci_socket);

    if (_attrib != NULL) {
        g_attrib_unref(_attrib);
    }
}

void
GATTRequester::on_notification(const uint16_t handle, const std::string data) {
    std::cout << "on notification, handle: 0x" << std::hex << handle << " -> ";

    for (std::string::const_iterator i=data.begin() + 2; i!=data.end(); i++) {
        printf("%02x:", int(*i));
    }
    printf("\n");
}

void
GATTRequester::on_indication(const uint16_t handle, const std::string data) {
    std::cout << "on indication, handle: 0x" << std::hex << handle << " -> ";

    for (std::string::const_iterator i=data.begin() + 2; i!=data.end(); i++) {
        printf("%02x:", int(*i));
    }
    printf("\n");
}

void
events_handler(const uint8_t* data, uint16_t size, gpointer userp) {
    GATTRequester* request = (GATTRequester*)userp;
    uint16_t handle = htobs(bt_get_le16(&data[1]));

    switch(data[0]) {
    case ATT_OP_HANDLE_NOTIFY:
        request->on_notification(handle, std::string((const char*)data, size));
        return;
    case ATT_OP_HANDLE_IND:
        request->on_indication(handle, std::string((const char*)data, size));
        break;
    default:
        throw std::runtime_error("Invalid event opcode!");
    }

    size_t plen;
    uint8_t* output = g_attrib_get_buffer(request->_attrib, &plen);
    uint16_t olen = enc_confirmation(output, plen);

    if (olen > 0)
        g_attrib_send(request->_attrib, 0, output, olen, NULL, NULL, NULL);
}

void
connect_cb(GIOChannel* channel, GError* err, gpointer userp) {
    GATTRequester* request = (GATTRequester*)userp;

    if (err) {
        request->_state = GATTRequester::STATE_DISCONNECTED;
        std::string msg(err->message);
        g_error_free(err);
        throw std::runtime_error(msg);
    }

    GError *gerr = NULL;
    uint16_t mtu;
    uint16_t cid;
    bt_io_get(channel, &gerr,
              BT_IO_OPT_IMTU, &mtu,
              BT_IO_OPT_CID, &cid,
              BT_IO_OPT_INVALID);

    // Can't detect MTU, using default
    if (gerr) {
        g_error_free(gerr);
        mtu = ATT_DEFAULT_LE_MTU;
    }

    if (cid == ATT_CID)
        mtu = ATT_DEFAULT_LE_MTU;

    request->_attrib = g_attrib_new(channel, mtu);

    g_attrib_register(request->_attrib, ATT_OP_HANDLE_NOTIFY, GATTRIB_ALL_HANDLES,
                      events_handler, userp, NULL);
    g_attrib_register(request->_attrib, ATT_OP_HANDLE_IND, GATTRIB_ALL_HANDLES,
                      events_handler, userp, NULL);

    request->_state = GATTRequester::STATE_CONNECTED;
}

gboolean
disconnect_cb(GIOChannel* channel, GIOCondition cond, gpointer userp) {
    GATTRequester* request = (GATTRequester*)userp;
    request->disconnect();
    return false;
}

void
GATTRequester::connect(bool wait,
		std::string channel_type, std::string security_level) {
    if (_state != STATE_DISCONNECTED)
        throw std::runtime_error("Already connecting or connected");

    _state = STATE_CONNECTING;

    GError *gerr = NULL;
    _channel = gatt_connect
        (_device.c_str(),        // 'hciX'
         _address.c_str(),       // 'mac address'
         channel_type.c_str(),   // 'public' '[public | random]'
         security_level.c_str(), // sec_level, '[low | medium | high]'
         0,                      // 0, psm
         0,                      // 0, mtu
         connect_cb,
         &gerr,
         (gpointer)this);

    if (_channel == NULL) {
        _state = STATE_DISCONNECTED;

        std::string msg(gerr->message);
        g_error_free(gerr);
        throw std::runtime_error(msg);
    }

    g_io_add_watch(_channel, G_IO_HUP, disconnect_cb, (gpointer)this);
    if (wait)
        check_channel();
}

bool
GATTRequester::is_connected() {
    return _state == STATE_CONNECTED;
}

void
GATTRequester::disconnect() {
    if (_state == STATE_DISCONNECTED)
        return;

    g_attrib_unref(_attrib);
    _attrib = NULL;

    g_io_channel_shutdown(_channel, false, NULL);
    g_io_channel_unref(_channel);
    _channel = NULL;

    _state = STATE_DISCONNECTED;
}

static void
read_by_handler_cb(guint8 status, const guint8* data, guint16 size, gpointer userp) {

    // Note: first byte is the payload size, remove it
    GATTResponse* response = (GATTResponse*)userp;
    response->on_response(std::string((const char*)data + 1, size - 1));
    response->notify(status);
}

void
GATTRequester::read_by_handle_async(uint16_t handle, GATTResponse* response) {
    check_channel();
    gatt_read_char(_attrib, handle, read_by_handler_cb, (gpointer)response);
}

boost::python::list
GATTRequester::read_by_handle(uint16_t handle) {
    GATTResponse response;
    read_by_handle_async(handle, &response);

    if (not response.wait(MAX_WAIT_FOR_PACKET))
        // FIXME: now, response is deleted, but is still registered on
        // GLIB as callback!!
        throw std::runtime_error("Device is not responding!");

    return response.received();
}

static void
read_by_uuid_cb(guint8 status, const guint8* data, guint16 size, gpointer userp) {
    struct att_data_list* list;
    list = dec_read_by_type_resp(data, size);
    if (list == NULL)
        return;

    GATTResponse* response = (GATTResponse*)userp;
    for (int i=0; i<list->num; i++) {
        uint8_t* item = list->data[i];

        // Remove handle addr
        item += 2;

        std::string value((const char*)item, list->len - 2);
        response->on_response(value);
    }

    att_data_list_free(list);
    response->notify(status);
}

void
GATTRequester::read_by_uuid_async(std::string uuid, GATTResponse* response) {
    uint16_t start = 0x0001;
    uint16_t end = 0xffff;
    bt_uuid_t btuuid;

    check_channel();
    if (bt_string_to_uuid(&btuuid, uuid.c_str()) < 0)
        throw std::runtime_error("Invalid UUID\n");

    gatt_read_char_by_uuid(_attrib, start, end, &btuuid, read_by_uuid_cb,
                           (gpointer)response);

}

boost::python::list
GATTRequester::read_by_uuid(std::string uuid) {
    PyThreadsGuard guard;
    GATTResponse response;

    read_by_uuid_async(uuid, &response);

    if (not response.wait(MAX_WAIT_FOR_PACKET))
        // FIXME: now, response is deleted, but is still registered on
        // GLIB as callback!!
        throw std::runtime_error("Device is not responding!");

    return response.received();
}

static void
write_by_handle_cb(guint8 status, const guint8* data, guint16 size, gpointer userp) {
    GATTResponse* response = (GATTResponse*)userp;
    response->on_response(std::string((const char*)data, size));
    response->notify(status);
}

void
GATTRequester::write_by_handle_async(uint16_t handle, std::string data,
                                     GATTResponse* response) {
    check_channel();
    gatt_write_char(_attrib, handle, (const uint8_t*)data.data(), data.size(),
                    write_by_handle_cb, (gpointer)response);
}

boost::python::list
GATTRequester::write_by_handle(uint16_t handle, std::string data) {
    PyThreadsGuard guard;
    GATTResponse response;

    write_by_handle_async(handle, data, &response);

    if (not response.wait(MAX_WAIT_FOR_PACKET))
        // FIXME: now, response is deleted, but is still registered on
        // GLIB as callback!!
        throw std::runtime_error("Device is not responding!");

    return response.received();
}

void
GATTRequester::check_channel() {
    time_t ts = time(NULL);
    bool should_update = false;

    while (_channel == NULL || _attrib == NULL) {
        should_update = true;

        usleep(1000);
        if (time(NULL) - ts > MAX_WAIT_FOR_PACKET)
            throw std::runtime_error("Channel or attrib not ready");
    }

    if (should_update) {
        // Update connection settings (supervisor timeut > 0.42 s)
        int l2cap_sock = g_io_channel_unix_get_fd(_channel);
        struct l2cap_conninfo info;
        socklen_t info_size = sizeof(info);

        getsockopt(l2cap_sock, SOL_L2CAP, L2CAP_CONNINFO, &info, &info_size);
        int handle = info.hci_handle;

        int retval = hci_le_conn_update(_hci_socket, handle, 24, 40, 0, 700, 25000);
        if (retval < 0) {
            std::string msg = std::string("Could not update HCI connection: ") +
                std::string(strerror(errno));
            throw std::runtime_error(msg);
        }
    }
}

static void
discover_primary_cb(guint8 status, GSList *services, void *userp) {
	GATTResponse* response = (GATTResponse*)userp;
	GSList *l;
	
	if (status) {
		std::string msg = std::string("Discover all primary services failed: %s\n");
		msg += att_ecode2str(status);
		throw std::runtime_error(msg);
	}
	
	if (services == NULL) {
		throw std::runtime_error(std::string("No primary service found\n"));
	}
	
	for (l = services; l; l = l->next) {
		struct gatt_primary *prim = (gatt_primary*)l->data;
		boost::python::dict sdescr;
		sdescr["start"] = prim->range.start;
		sdescr["end"] = prim->range.end;
		sdescr["uuid"] = prim->uuid;
		response->on_response(sdescr);
	}
	
	response->notify(status);
}


void
GATTRequester::discover_primary_async(GATTResponse* response){
	gatt_discover_primary(_attrib, NULL, discover_primary_cb, (gpointer)response);
}

boost::python::list GATTRequester::discover_primary() {
	GATTResponse response;
	discover_primary_async(&response);
	
	if (not response.wait(5*MAX_WAIT_FOR_PACKET))
        // FIXME: now, response is deleted, but is still registered on
        // GLIB as callback!!
		throw std::runtime_error("Device is not responding!");
	return response.received();

}

/* Characteristics Discovery

 */
static void 
discover_char_cb(guint8 status, GSList *characteristics, void *user_data){
	GATTResponse* response = (GATTResponse*)user_data;
	GSList *l;
	
	if (status) {
		std::string msg = std::string("Discover all characteristics failed: ");
		msg += att_ecode2str(status);
		throw std::runtime_error(msg);
	}
	
	if (characteristics == NULL) {
		throw std::runtime_error(std::string("No characteristics found\n"));
	}
	
	for (l = characteristics; l; l = l->next) {
		struct gatt_char *chars = (gatt_char*)l->data;
#if 1
		boost::python::dict adescr;
		adescr["handle"] = chars->handle;
		adescr["properties"] = chars->properties;
		adescr["value_handle"] = chars->value_handle;
		adescr["uuid"] = chars->uuid;
		response->on_response(adescr);
#else
		std::stringstream value;
		value << "Attr_handle:"<<std::hex<<std::showbase<< chars->handle <<",";
		value <<"properties:"<<std::hex<<std::showbase<<chars->properties<<",";
		value <<"value handle:"<<std::hex<<std::showbase<<chars->value_handle<<",";
		value <<"UUID:"<<chars->uuid;
		response->on_response(value.str());
#endif
	}
	
	response->notify(status);
}

void GATTRequester::discover_characteristics_async(GATTResponse* response, int start, int end, std::string uuid_str) {

	bt_uuid_t uuid;

	if (uuid_str.size()== 0){
		gatt_discover_char(_attrib, start, end, NULL, discover_char_cb, (gpointer)response);
	}
	else if (bt_string_to_uuid(&uuid, uuid_str.c_str())<0){
		throw std::runtime_error(std::string("Invalid UUID"));
	} else {
		gatt_discover_char(_attrib, start, end, &uuid, discover_char_cb, (gpointer)response);
	}
}

boost::python::list GATTRequester::discover_characteristics(int start, int end, std::string uuid_str) {
	GATTResponse response;
	discover_characteristics_async(&response, start, end, uuid_str);
	
	if (not response.wait(5*MAX_WAIT_FOR_PACKET))
        // FIXME: now, response is deleted, but is still registered on
        // GLIB as callback!!
		throw std::runtime_error("Device is not responding!");
	return response.received();

}

