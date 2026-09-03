#pragma once

#include "Event.h"


namespace SunEngine
{


    class KeyPressedEvent : public Event
    {

    private:

        int KeyCode;



    public:

        KeyPressedEvent(int key)
            :
            KeyCode(key)
        {

        }



        int GetKeyCode() const
        {
            return KeyCode;
        }



        EventType GetEventType() const override
        {
            return EventType::KeyPressed;
        }


    };



    class KeyReleasedEvent : public Event
    {

    private:

        int KeyCode;



    public:

        KeyReleasedEvent(int key)
            :
            KeyCode(key)
        {

        }



        int GetKeyCode() const
        {
            return KeyCode;
        }



        EventType GetEventType() const override
        {
            return EventType::KeyReleased;
        }


    };


}