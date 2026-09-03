#pragma once

#include "Event.h"


namespace SunEngine
{

    class WindowCloseEvent : public Event
    {

    public:

        WindowCloseEvent()
        {

        }


        EventType GetEventType() const override
        {
            return EventType::WindowClose;
        }

    };



    class WindowResizeEvent : public Event
    {

    private:

        unsigned int Width;
        unsigned int Height;


    public:

        WindowResizeEvent(
            unsigned int width,
            unsigned int height
        )
            :
            Width(width),
            Height(height)
        {

        }


        unsigned int GetWidth() const
        {
            return Width;
        }


        unsigned int GetHeight() const
        {
            return Height;
        }


        EventType GetEventType() const override
        {
            return EventType::WindowResize;
        }

    };


}