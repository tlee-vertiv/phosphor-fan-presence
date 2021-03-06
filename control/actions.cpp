#include "actions.hpp"
#include "utility.hpp"

namespace phosphor
{
namespace fan
{
namespace control
{
namespace action
{

using namespace phosphor::fan;

Action call_actions_based_on_timer(TimerConf&& tConf,
                                   std::vector<Action>&& actions)
{
    return [tConf = std::move(tConf),
            actions = std::move(actions)](control::Zone& zone,
                                          const Group& group)
    {
        try
        {
            auto it = zone.getTimerEvents().find(__func__);
            if (it != zone.getTimerEvents().end())
            {
                auto& timers = it->second;
                auto timerIter = zone.findTimer(group, actions, timers);
                if (timerIter == timers.end())
                {
                    // No timer exists yet for action, add timer
                    zone.addTimer(__func__, group, actions, tConf);
                }
                else if (timerIter != timers.end())
                {
                    // Remove any timer for this group
                    timers.erase(timerIter);
                    if (timers.empty())
                    {
                        zone.getTimerEvents().erase(it);
                    }
                }
            }
            else
            {
                // No timer exists yet for event, add timer
                zone.addTimer(__func__, group, actions, tConf);
            }
        }
        catch (const std::out_of_range& oore)
        {
            // Group not found, no timers set
        }
    };
}

void default_floor_on_missing_owner(Zone& zone, const Group& group)
{
    // Set/update the services of the group
    zone.setServices(&group);
    auto services = zone.getGroupServices(&group);
    auto defFloor = std::any_of(
        services.begin(),
        services.end(),
        [](const auto& s)
        {
            return !std::get<hasOwnerPos>(s);
        });
    if (defFloor)
    {
        zone.setFloor(zone.getDefFloor());
    }
    // Update fan control floor change allowed
    zone.setFloorChangeAllow(&group, !defFloor);
}

Action set_speed_on_missing_owner(uint64_t speed)
{
    return [speed](control::Zone& zone, const Group& group)
    {
        // Set/update the services of the group
        zone.setServices(&group);
        auto services = zone.getGroupServices(&group);
        auto missingOwner = std::any_of(
            services.begin(),
            services.end(),
            [](const auto& s)
            {
                return !std::get<hasOwnerPos>(s);
            });
        if (missingOwner)
        {
            zone.setSpeed(speed);
        }
        // Update group's fan control active allowed based on action results
        zone.setActiveAllow(&group, !missingOwner);
    };
}

void set_request_speed_base_with_max(control::Zone& zone,
                                     const Group& group)
{
    int64_t base = 0;
    std::for_each(
            group.begin(),
            group.end(),
            [&zone, &base](auto const& entry)
        {
            try
            {
                auto value = zone.template getPropertyValue<int64_t>(
                        std::get<pathPos>(entry),
                        std::get<intfPos>(entry),
                        std::get<propPos>(entry));
                base = std::max(base, value);
            }
            catch (const std::out_of_range& oore)
            {
                // Property value not found, base request speed unchanged
            }
        });
    // A request speed base of 0 defaults to the current target speed
    zone.setRequestSpeedBase(base);
}

Action set_floor_from_average_sensor_value(
        std::map<int64_t, uint64_t>&& val_to_speed)
{
    return [val_to_speed = std::move(val_to_speed)](control::Zone& zone,
                                                    const Group& group)
    {
        auto speed = zone.getDefFloor();
        if (group.size() != 0)
        {
            auto count = 0;
            auto sumValue = std::accumulate(
                    group.begin(),
                    group.end(),
                    0,
                    [&zone, &count](int64_t sum, auto const& entry)
                    {
                        try
                        {
                            return sum +
                                zone.template getPropertyValue<int64_t>(
                                    std::get<pathPos>(entry),
                                    std::get<intfPos>(entry),
                                    std::get<propPos>(entry));
                        }
                        catch (const std::out_of_range& oore)
                        {
                            count++;
                            return sum;
                        }
                    });
            if ((group.size() - count) > 0)
            {
                auto groupSize = static_cast<int64_t>(group.size());
                auto avgValue = sumValue / (groupSize - count);
                auto it = std::find_if(
                    val_to_speed.begin(),
                    val_to_speed.end(),
                    [&avgValue](auto const& entry)
                    {
                        return avgValue < entry.first;
                    }
                );
                if (it != std::end(val_to_speed))
                {
                    speed = (*it).second;
                }
            }
        }
        zone.setFloor(speed);
    };
}

Action set_ceiling_from_average_sensor_value(
        std::map<int64_t, uint64_t>&& val_to_speed)
{
    return [val_to_speed = std::move(val_to_speed)](Zone& zone,
                                                    const Group& group)
    {
        auto speed = zone.getCeiling();
        if (group.size() != 0)
        {
            auto count = 0;
            auto sumValue = std::accumulate(
                    group.begin(),
                    group.end(),
                    0,
                    [&zone, &count](int64_t sum, auto const& entry)
                    {
                        try
                        {
                            return sum +
                                zone.template getPropertyValue<int64_t>(
                                    std::get<pathPos>(entry),
                                    std::get<intfPos>(entry),
                                    std::get<propPos>(entry));
                        }
                        catch (const std::out_of_range& oore)
                        {
                            count++;
                            return sum;
                        }
                    });
            if ((group.size() - count) > 0)
            {
                auto groupSize = static_cast<int64_t>(group.size());
                auto avgValue = sumValue / (groupSize - count);
                auto prevValue = zone.swapCeilingKeyValue(avgValue);
                if (avgValue != prevValue)
                {// Only check if previous and new values differ
                    if (avgValue < prevValue)
                    {// Value is decreasing from previous
                        for (auto it = val_to_speed.rbegin();
                             it != val_to_speed.rend();
                             ++it)
                        {
                            if (it == val_to_speed.rbegin() &&
                                avgValue >= it->first)
                            {
                                // Value is at/above last map key, set
                                // ceiling speed to the last map key's value
                                speed = it->second;
                                break;
                            }
                            else if (std::next(it, 1) == val_to_speed.rend() &&
                                     avgValue <= it->first)
                            {
                                // Value is at/below first map key, set
                                // ceiling speed to the first map key's value
                                speed = it->second;
                                break;
                            }
                            if (avgValue < it->first &&
                                it->first <= prevValue)
                            {
                                // Value decreased & transitioned across
                                // a map key, update ceiling speed to this
                                // map key's value when new value is below
                                // map's key and the key is at/below the
                                // previous value
                                speed = it->second;
                            }
                        }
                    }
                    else
                    {// Value is increasing from previous
                        for (auto it = val_to_speed.begin();
                             it != val_to_speed.end();
                             ++it)
                        {
                            if (it == val_to_speed.begin() &&
                                avgValue <= it->first)
                            {
                                // Value is at/below first map key, set
                                // ceiling speed to the first map key's value
                                speed = it->second;
                                break;
                            }
                            else if (std::next(it, 1) == val_to_speed.end() &&
                                     avgValue >= it->first)
                            {
                                // Value is at/above last map key, set
                                // ceiling speed to the last map key's value
                                speed = it->second;
                                break;
                            }
                            if (avgValue > it->first &&
                                it->first >= prevValue)
                            {
                                // Value increased & transitioned across
                                // a map key, update ceiling speed to this
                                // map key's value when new value is above
                                // map's key and the key is at/above the
                                // previous value
                                speed = it->second;
                            }
                        }
                    }
                }
            }
        }
        zone.setCeiling(speed);
    };
}

Action set_floor_from_median_sensor_value(
        int64_t lowerBound,
        int64_t upperBound,
        std::map<int64_t, uint64_t>&& valueToSpeed)
{
    return [lowerBound,
            upperBound,
            valueToSpeed = std::move(valueToSpeed)](control::Zone& zone,
                                                    const Group& group)
    {
        auto speed = zone.getDefFloor();
        if (group.size() != 0)
        {
            std::vector<int64_t> validValues;
            for (auto const& member : group)
            {
                try
                {
                    auto value = zone.template getPropertyValue<int64_t>(
                            std::get<pathPos>(member),
                            std::get<intfPos>(member),
                            std::get<propPos>(member));
                    if (value == std::clamp(value, lowerBound, upperBound))
                    {
                        // Sensor value is valid
                        validValues.emplace_back(value);
                    }
                }
                catch (const std::out_of_range& oore)
                {
                    continue;
                }
            }

            if (!validValues.empty())
            {
                auto median = validValues.front();
                // Get the determined median value
                if (validValues.size() == 2)
                {
                    // For 2 values, use the highest instead of the average
                    // for a thermally safe floor
                    median = *std::max_element(validValues.begin(),
                                               validValues.end());
                }
                else if (validValues.size() > 2)
                {
                    median = utility::getMedian(validValues);
                }

                // Use determined median sensor value to find floor speed
                auto it = std::find_if(
                    valueToSpeed.begin(),
                    valueToSpeed.end(),
                    [&median](auto const& entry)
                    {
                        return median < entry.first;
                    }
                );
                if (it != std::end(valueToSpeed))
                {
                    speed = (*it).second;
                }
            }
        }
        zone.setFloor(speed);
    };
}

} // namespace action
} // namespace control
} // namespace fan
} // namespace phosphor
