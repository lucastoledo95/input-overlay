/**
 * This file is part of input-overlay
 * which is licensed under the MIT license.
 * See LICENSE or mozilla.org/en-US/MPL/2.0/
 * github.com/univrsal/input-overlay
 */

#include "../../ccl/ccl.hpp"
#include "overlay.hpp"
#include "layout_constants.hpp"
#include "element/element_button.hpp"
#include "element/element_data_holder.hpp"
#include "element/element.hpp"
#include "element/element_mouse_wheel.hpp"
#include "element/element_trigger.hpp"
#include "element/element_analog_stick.hpp"
#include "../sources/input_source.hpp"
#include "element/element_gamepad_id.hpp"
#include "element/element_dpad.hpp"
#include "network/remote_connection.hpp"
#include "network/io_server.hpp"
#include "element/element_mouse_movement.hpp"

extern "C" {
#include <graphics/image-file.h>
}

namespace sources
{
    class shared_settings;
}

//namespace Layout {

overlay::~overlay()
{
    unload();
}

overlay::overlay(sources::shared_settings* settings)
{
    m_settings = settings;
    m_is_loaded = load();
}

bool overlay::load()
{
    unload();
    const auto image_loaded = load_texture();
    m_is_loaded = image_loaded && load_cfg();

    if (!m_is_loaded)
    {
        m_settings->gamepad = 0;
        if (!image_loaded)
        {
            m_settings->cx = 100; /* Default size */
            m_settings->cy = 100;
        }
    }

    return m_is_loaded;
}

void overlay::unload()
{
    unload_texture();
    unload_elements();
    m_data.clear();
    m_settings->gamepad = 0;
    m_settings->cx = 100;
    m_settings->cy = 100;
}

bool overlay::load_cfg()
{
    if (!m_settings || m_settings->layout_file.empty())
        return false;

    auto cfg = new ccl_config(m_settings->layout_file, "");
    auto flag = true;

    if (!cfg->has_fatal_errors())
    {
        m_settings->cx = static_cast<uint32_t>(cfg->get_int(CFG_TOTAL_WIDTH, true));
        m_settings->cy = static_cast<uint32_t>(cfg->get_int(CFG_TOTAL_HEIGHT, true));

        auto element_id = cfg->get_string(CFG_FIRST_ID);
        const auto debug_mode = cfg->get_bool(CFG_DEBUG_FLAG, true);

#ifndef _DEBUG
        if (debug_mode)
        {
#else
        {
#endif
            blog(LOG_INFO, "[input-overlay] Started loading of %s",
                m_settings->layout_file.c_str());
        }

        while (!element_id.empty())
        {
            load_element(cfg, element_id, debug_mode);
            element_id = cfg->get_string(element_id + CFG_NEXT_ID, true);
        }
    }

    if (cfg->has_errors())
    {
        blog(LOG_WARNING, "[input-overlay] %s", cfg->get_error_message().c_str());
        if (cfg->has_fatal_errors())
        {
            blog(LOG_WARNING, "Fatal errors occured while loading config file");
            flag = false;
        }
        else
        {
            /* Populate data map */
            for (auto const& element : m_elements)
            {
                element_data* data = nullptr;

                switch (element->get_type())
                {
                case BUTTON:
                    data = new element_data_button(STATE_RELEASED);
                    break;
                case MOUSE_SCROLLWHEEL:
                    data = new element_data_wheel(STATE_RELEASED);
                    break;
                case TRIGGER:
                    data = new element_data_trigger(0.f, 0.f);
                    break;
                case ANALOG_STICK:
                    data = new element_data_analog_stick(STATE_RELEASED, STATE_RELEASED, 0.f, 0.f, 0.f, 0.f);
                    break;
                case DPAD_STICK:
                    data = new element_data_dpad(DPAD_LEFT, STATE_RELEASED);
                    break;
                case MOUSE_MOVEMENT:
                    data = new element_data_mouse_stats(0, 0);
                    break;
                default:;
                }

                if (data)
                    m_data[element->get_keycode()] = std::unique_ptr<element_data>(data);
            }
        }
    }

    delete cfg;
    return flag;
}

bool overlay::load_texture()
{
    if (!m_settings || m_settings->image_file.empty())
        return false;

    auto flag = true;

    if (m_image == nullptr)
    {
        m_image = new gs_image_file_t();
    }

    gs_image_file_init(m_image, m_settings->image_file.c_str());

    obs_enter_graphics();
    gs_image_file_init_texture(m_image);
    obs_leave_graphics();

    if (!m_image->loaded)
    {
        blog(LOG_WARNING, "[input-overlay] Error: failed to load texture %s", m_settings->image_file.c_str());
        flag = false;
    }
    else
    {
        m_settings->cx = m_image->cx;
        m_settings->cy = m_image->cy;
    }

    return flag;
}

void overlay::unload_texture() const
{
    obs_enter_graphics();
    gs_image_file_free(m_image);
    obs_leave_graphics();
}

void overlay::unload_elements()
{
    m_elements.clear();
}

void overlay::draw(gs_effect_t* effect)
{
    if (m_is_loaded)
    {
        for (auto const& element : m_elements)
        {
            const auto data = m_data[element->get_keycode()].get();
            element->draw(effect, m_image, data, m_settings);
        }
    }
}

void overlay::refresh_data()
{
    /* This copies over necessary element data information
     * to make sure the overlay always has data available to
     * draw the overlay. If the data was directly accessed in the render
     * method, the overlay can start to flicker if the frame is rendered
     * while the data is currently inaccessible, because it is being written
     * to by the input thread, resulting in all buttons being unpressed
     */

    element_data_holder* source = nullptr;
    std::lock_guard<std::mutex> lck1(hook::mutex);
    std::lock_guard<std::mutex> lck2(network::mutex);

    if (hook::data_initialized || network::network_flag)
    {
        if (network::server_instance && m_settings->selected_source > 0)
        {
            source = network::server_instance->
                get_client(m_settings->selected_source - 1)->get_data();
        }
        else
        {
            source = hook::input_data;
        }
    }

    if (source)
    {
        for (auto const& element : m_elements)
        {
            element_data* data = nullptr;

            if (source)
            {
                switch (element->get_source())
                {
                case GAMEPAD:
                    data = source->get_by_gamepad(m_settings->gamepad,
                        element->get_keycode());
                    break;
                default:
                case MOUSE_POS:;
                case DEFAULT:
                    data = source->get_by_code(element->get_keycode());
                    break;
                case NONE:;
                }
            }

            if (m_data[element->get_keycode()] != nullptr)
            {
                m_data[element->get_keycode()]->merge(data);
            }
        }
    }
}

void overlay::load_element(ccl_config* cfg, const std::string& id, const bool debug)
{
    const auto type = cfg->get_int(id + CFG_TYPE);
    element* new_element = nullptr;

    switch (type)
    {
    case TEXTURE:
        new_element = new element_texture();
        break;
    case BUTTON:
        new_element = new element_button();
        break;
    case MOUSE_SCROLLWHEEL:
        new_element = new element_wheel();
        break;
    case TRIGGER:
        new_element = new element_trigger();
        break;
    case ANALOG_STICK:
        new_element = new element_analog_stick();
        break;
    case GAMEPAD_ID:
        new_element = new element_gamepad_id();
        break;
    case DPAD_STICK:
        new_element = new element_dpad();
        break;
    case MOUSE_MOVEMENT:
        new_element = new element_mouse_movement();
        break;
    case TEXT:
        /* TODO: text element*/
        break;
    default:
        if (debug)
            blog(LOG_INFO, "Invalid element type %i for %s",
                type, id.c_str());
    }

    if (new_element)
    {
        new_element->load(cfg, id);
        m_elements.emplace_back(new_element);

#ifndef _DEBUG
        if (debug)
        {
#else
        {
#endif
            blog(LOG_INFO, " Type: %14s, KEYCODE: 0x%4X ID: %s",
                element_type_to_string(static_cast<element_type>(type)), new_element->get_keycode(), id.c_str());
        }
    }
}

const char* overlay::element_type_to_string(const element_type t)
{
    switch (t)
    {
    case TEXTURE: return "Texture";
    case BUTTON: return "Button";
    case ANALOG_STICK: return "Analog stick";
    case MOUSE_SCROLLWHEEL: return "Scroll wheel";
    case MOUSE_MOVEMENT: return "Mouse movement";
    case TRIGGER: return "Trigger";
    case GAMEPAD_ID: return "Gamepad ID";
    case TEXT: return "Text";
    case DPAD_STICK: return "DPad";
    default:
    case INVALID: return "Invalid";
    }
}

//};
