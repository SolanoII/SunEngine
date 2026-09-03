#pragma once

namespace SunEngine
{

    enum class EventType
    {
        None = 0,

        WindowClose,
        WindowResize,

        KeyPressed,
        KeyReleased
    };


    class Event
    {

    public:

        virtual ~Event() = default;


        virtual EventType GetEventType() const = 0;


        bool Handled = false;

    };


}