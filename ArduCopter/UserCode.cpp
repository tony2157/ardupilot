#include "Copter.h"
#include <utility>
#include <SRV_Channel/SRV_Channel.h>
#include <Filter/LPFrd.h>

//Humidity sensor Params
const int N_RH = 4;     //supports up to 4

//IMET sensor Params
const int N_imet = 4;   //supports up to 4

//Fan control params
//It will run the fan at SERVO_MAX, which can be set in the params list
uint16_t fan_pwm_on = 100;
uint16_t fan_pwm_off = 0;
bool _fan_status;

//Wind estimator Params
float _wind_speed, _wind_dir;
float R13, R23, R33;
float last_yrate;
bool high_wind_flag;
uint32_t wvane_now;
//Declare digital LPF
LPFrdFloat filt_thrvec_x;
LPFrdFloat filt_thrvec_y;
LPFrdFloat filt_thrvec_z;
//g.wind_vane_wsA -> Coefficient A of the linear wind speed equation, from calibration
//g.wind_vane_wsB -> Coefficient B of the linear wind speed equation, from calibration
//g.wind_vane_min_roll -> Minimum roll angle that the wind vane will correct (too low and the copter will oscilate)
//g.wind_vane_fine_rate -> Maximum yaw angle rate at which the Copter will rotate
//g.wind_vane_fine_gain -> Wind vane gain: higher values will increase the resposivness
//g.wind_vane_fs -> Wind vane sampling frequency, range 1 to 10Hz.

//Smart Vertical Profiling Battery monitor parameters
float Whc; // Energy consumed
float Whn; // Energy needed to fly home safely
float int_wvspd; // Wind speed history or memory while ascending
uint32_t vpbatt_now;
bool batt_home_ok;
bool batt_warning_flag;

//AutoVP mission generation
uint32_t mission_now;


#ifdef USERHOOK_INIT
void Copter::userhook_init()
{
    // put your initialisation code here
    // this will be called once at start-up

    //Initialize Wind estimator
    _wind_dir = 0.0f;  _wind_speed = 0.0f;
    R13 = 0.0f; R23 = 0.0f;
    last_yrate = 0;
    high_wind_flag = false;
    wvane_now = AP_HAL::millis();

    //AutoVP initialize
    mission_now = AP_HAL::millis();

    //Wind filter initialization
    float Fss;
    if(is_zero(fmodf(10,g2.user_parameters.get_wvane_fs()))){
        Fss = g2.user_parameters.get_wvane_fs();
    }
    else{
        Fss = 10.0f;
    }
    if(g2.user_parameters.get_wvane_cutoff() < 0.05){
        //Min Fc = 0.05 for stable yaw
        filt_thrvec_x.set_cutoff_frequency(Fss,0.05);
        filt_thrvec_y.set_cutoff_frequency(Fss,0.05);
        filt_thrvec_z.set_cutoff_frequency(Fss,0.05);
    }
    else{
        //Initialize Butterworth filter
        filt_thrvec_x.set_cutoff_frequency(Fss,g2.user_parameters.get_wvane_cutoff());
        filt_thrvec_y.set_cutoff_frequency(Fss,g2.user_parameters.get_wvane_cutoff());
        filt_thrvec_z.set_cutoff_frequency(Fss,g2.user_parameters.get_wvane_cutoff());
    }

    //VPBatt_monitor initilize
    Whc = Whn = int_wvspd = 0;
    batt_home_ok = true;
    batt_warning_flag = false;
    vpbatt_now = AP_HAL::millis();

    // Initialize Fan Control
    SRV_Channels::set_output_scaled(SRV_Channel::k_egg_drop, fan_pwm_off);
    _fan_status = false;
}
#endif

#ifdef USERHOOK_FASTLOOP
void Copter::userhook_FastLoop()
{
    // put your 100Hz code here
}
#endif

#ifdef USERHOOK_50HZLOOP
void Copter::userhook_50Hz()
{
    // Smart Battery Management - Use it only for Vertical profiling
    float alt;
    float dt = (float)(AP_HAL::millis() - vpbatt_now);
    // Compute loop only when flying
    if(!ap.land_complete){
        // Enter loop every 100 milliseconds
        if(dt >= 100.0f){
            // Calculate energy consumed in Watt-hour
            float current;
            if(battery.current_amps(current)){
                Whc = Whc + battery.voltage()*current*dt/3.6e6f;
            }

            //Compute the temporal integration of the wind speed (works as a memory)
            Vector3f velocity;
            copter.ahrs.get_velocity_NED(velocity);
            if(velocity[2] < 0){
                int_wvspd = int_wvspd + _wind_speed*dt/1000;
            }

            // Calculate the Descent-Energy-consumption per meter height (function of wind speed)
            float Whm = 1.5e-6f*int_wvspd + 7e-3;
            // Constrain lower values
            Whm = Whm > 0.01 ? Whm : 0.01;
            
            // Get current altitude in meters
            copter.ahrs.get_relative_position_D_home(alt);
            alt = -1.0f*alt;
            // Calculate energy needed to get home safely
            Whn = Whm*alt;
            
            // Estimate the total energy used (percentage)
            // vpbatt_reserve is the desired batt percentage after landing
            float Wh_tot = (Whc + Whn)/g2.user_parameters.get_vpbatt_wh() + g2.user_parameters.get_vpbatt_reserve()/100.0f + 0.05f;

            //Switch to RTL automatically if battery reaches critical remaining energy
            if(!is_zero(g2.user_parameters.get_vpbatt_wh())){
                // Issue a warning once when the battery altitude range is over 85%
                if(Wh_tot >= 0.85f && batt_warning_flag == false){
                    // It will still warn, even if the function is disabled
                    if(!is_zero(g2.user_parameters.get_vpbatt_enabled())){
                        gcs().send_text(MAV_SEVERITY_WARNING, "Over 85 Batt range");
                    }
                    batt_warning_flag = true;
                }

                // Trigger RTL when max battery altitude range is reached
                if(Wh_tot >= 1.0f && batt_home_ok == true){
                    gcs().send_text(MAV_SEVERITY_WARNING, "Max Batt range: Switch to RTL");
                    // It will still warn, even if the function is disabled
                    if(!is_zero(g2.user_parameters.get_vpbatt_enabled())){
                        copter.set_mode(Mode::Number::RTL, ModeReason::UNKNOWN);
                    }
                    batt_home_ok = false;
                }
            }
            // Update time
            vpbatt_now = AP_HAL::millis();

            //Print on terminal for debugging
            printf("Whc: %5.2f \n",Whc);
            printf("Vel_Z: %5.2f \n",velocity[2]);
            printf("int_wvspd: %5.4f \n",int_wvspd);
            printf("Whm: %5.4f \n",Whm);
            printf("Whn: %5.2f \n",Whn);
            printf("Wh_tot: %5.2f \n",Wh_tot);
        }
    }
    else{
        vpbatt_now = AP_HAL::millis();
        int_wvspd = 0.0f;
    }
}
#endif

#ifdef USERHOOK_MEDIUMLOOP
void Copter::userhook_MediumLoop()
{
    #if CONFIG_HAL_BOARD != HAL_BOARD_SITL
    // Read Temperature, Resistance and Health. Write sensors packet into the SD card
    // Temperature Data Logger ///////////////////////////////////////////////////////////////////////////////////////////
    struct log_IMET pkt_temp = {
        LOG_PACKET_HEADER_INIT(LOG_IMET_MSG),
        time_stamp             : AP_HAL::micros64(),                //Store time in microseconds
        fan_status             : _fan_status,                       //Store Fan on/off status
        healthy1               : copter.CASS_Imet[0].healthy(),     //Store sensor health
        healthy2               : copter.CASS_Imet[1].healthy(),
        healthy3               : copter.CASS_Imet[2].healthy(),
        healthy4               : copter.CASS_Imet[3].healthy(),
        temperature1           : copter.CASS_Imet[0].temperature(), //Store iMet Temperature
        temperature2           : copter.CASS_Imet[1].temperature(),
        temperature3           : copter.CASS_Imet[2].temperature(),
        temperature4           : copter.CASS_Imet[3].temperature(),
        resist1                : copter.CASS_Imet[0].resistance(),  //Store iMet bead resistence
        resist2                : copter.CASS_Imet[1].resistance(),
        resist3                : copter.CASS_Imet[2].resistance(),
        resist4                : copter.CASS_Imet[3].resistance()
    }; 
    #else
        float alt; float simT;
        copter.ahrs.get_relative_position_D_home(alt);
        alt = -1*alt;  
        if(alt < 900){
            simT = 2.99e-11f*powf(alt,4) - 3.70454e-8*powf(alt,3) - 3.86806e-6*powf(alt,2) + 1.388511e-2*alt + 287.66;
        }
        else{
            simT = 289.64f;
        }
        uint32_t m = AP_HAL::millis();
        // Write simulated sensors packet into the SD card
        struct log_IMET pkt_temp = {
            LOG_PACKET_HEADER_INIT(LOG_IMET_MSG),
            time_stamp             : AP_HAL::micros64(),
            fan_status             : _fan_status,
            healthy1               : copter.CASS_Imet[0].healthy(),
            healthy2               : copter.CASS_Imet[1].healthy(),
            healthy3               : copter.CASS_Imet[2].healthy(),
            healthy4               : copter.CASS_Imet[3].healthy(),
            temperature1           : simT + sinf(0.001f*float(m)) * 0.002f,
            temperature2           : simT + sinf(0.0007f*float(m)) * 0.003f,
            temperature3           : simT + sinf(0.0003f*float(m)) * 0.0025f,
            temperature4           : simT + sinf(0.0005f*float(m)) * 0.0028f,
            resist1                : -356.9892f*simT + 110935.3763f + sinf(0.001f*float(m)) * 0.002f,
            resist2                : -356.9892f*simT + 110935.3763f + sinf(0.0007f*float(m)) * 0.003f,
            resist3                : -356.9892f*simT + 110935.3763f + sinf(0.0003f*float(m)) * 0.0025f,
            resist4                : -356.9892f*simT + 110935.3763f + sinf(0.0005f*float(m)) * 0.0028f
        };
    #endif
    logger.WriteBlock(&pkt_temp, sizeof(pkt_temp));   //Send package to SD card
}
#endif

#ifdef USERHOOK_SLOWLOOP
void Copter::userhook_SlowLoop()
{
     #if CONFIG_HAL_BOARD != HAL_BOARD_SITL
        // Read Rel. Humidity, Temperature and Health. Write sensors packet into the SD card
        // Relative Humidity Data Logger ///////////////////////////////////////////////////////////////////////////////////////////
        struct log_RH pkt_RH = {
            LOG_PACKET_HEADER_INIT(LOG_RH_MSG),
            time_stamp             : AP_HAL::micros64(),                        //Store time in microseconds
            healthy1               : copter.CASS_HYT271[0].healthy(),           //Store senors health
            healthy2               : copter.CASS_HYT271[1].healthy(),
            healthy3               : copter.CASS_HYT271[2].healthy(),
            healthy4               : copter.CASS_HYT271[3].healthy(),
            humidity1              : copter.CASS_HYT271[0].relative_humidity(), //Store Rel. humidity
            humidity2              : copter.CASS_HYT271[1].relative_humidity(),
            humidity3              : copter.CASS_HYT271[2].relative_humidity(),
            humidity4              : copter.CASS_HYT271[3].relative_humidity(),
            RHtemp1                : copter.CASS_HYT271[0].temperature(),       //Store temperature
            RHtemp2                : copter.CASS_HYT271[1].temperature(),
            RHtemp3                : copter.CASS_HYT271[2].temperature(),
            RHtemp4                : copter.CASS_HYT271[3].temperature()
        };
    #else
        float alt; float simH;
        copter.ahrs.get_relative_position_D_home(alt);
        alt = -1*alt; 
        if(alt < 900){
            simH = -2.44407e-10f*powf(alt,4) + 3.88881064e-7*powf(alt,3) - 1.41943e-4*powf(alt,2) - 2.81895e-2*alt + 51.63;
        }
        else{
            simH = 34.44f;
        }
        uint32_t m = AP_HAL::millis();
        // Write sensors packet into the SD card
        struct log_RH pkt_RH = {
            LOG_PACKET_HEADER_INIT(LOG_RH_MSG),
            time_stamp             : AP_HAL::micros64(),
            healthy1               : copter.CASS_HYT271[0].healthy(),
            healthy2               : copter.CASS_HYT271[1].healthy(),
            healthy3               : copter.CASS_HYT271[2].healthy(),
            healthy4               : copter.CASS_HYT271[3].healthy(),
            humidity1              : simH + sinf(0.1f*float(m)) * 0.02f,
            humidity2              : simH + sinf(0.3f*float(m)) * 0.03f,
            humidity3              : simH + sinf(0.5f*float(m)) * 0.025f,
            humidity4              : simH + sinf(0.7f*float(m)) * 0.035f,
            RHtemp1                : 298.15f + sinf(0.0003f*m) * 2.0f,
            RHtemp2                : 298.15f + sinf(0.0004f*m) * 2.0f,
            RHtemp3                : 298.15f + sinf(0.0005f*m) * 2.0f,
            RHtemp4                : 298.15f + sinf(0.0006f*m) * 2.0f
        };
    #endif
    logger.WriteBlock(&pkt_RH, sizeof(pkt_RH));   //Send package to SD card
}
#endif

#ifdef USERHOOK_SUPERSLOWLOOP
void Copter::userhook_SuperSlowLoop()
{
    //Run algo after Copter takes off
    if(!ap.land_complete && copter.position_ok()){

        //Fan Control ////////////////////////////////////////////////////////////////////////////////////////

        //Get AGL altitude in cm
        float alt;
        copter.ahrs.get_relative_position_D_home(alt);
        alt = -100.0f*alt;
   
        //Smart fan on/off logic
        if(alt > 185.0f && SRV_Channels::get_output_scaled(SRV_Channel::k_egg_drop) < 50){
            SRV_Channels::set_output_scaled(SRV_Channel::k_egg_drop, fan_pwm_on);
            _fan_status = true;
            gcs().send_text(MAV_SEVERITY_INFO, "Scoop Fan activated");
            #if CONFIG_HAL_BOARD == HAL_BOARD_SITL
                printf("FAN ON \n");  //printf("PWM: %5.2f \n",var); //for debugging
            #endif
        }
        else{
            if(alt < 140.0f && SRV_Channels::get_output_scaled(SRV_Channel::k_egg_drop) > 50){
                SRV_Channels::set_output_scaled(SRV_Channel::k_egg_drop, fan_pwm_off);
                _fan_status = false;
                #if CONFIG_HAL_BOARD == HAL_BOARD_SITL
                    printf("FAN OFF \n");  
                #endif
            }
        }

        //Wind Estimator Algorithm //////////////////////////////////////////////////////////////////////////

        //Get thurst vector elements from the rotation matrix
        R13 = -1*copter.ahrs.get_rotation_body_to_ned().a.z;
        R23 = -1*copter.ahrs.get_rotation_body_to_ned().b.z;
        R33 = -1*copter.ahrs.get_rotation_body_to_ned().c.z;

        //Wind vane loop starts here. Loop frequency is defined by WVANE_FS param in Hz
        if((AP_HAL::millis() - wvane_now) >= (uint32_t)(1000/g2.user_parameters.get_wvane_fs())){
            //Apply Butterworth LPF on each element
            float thrvec_x, thrvec_y, thrvec_z;
            thrvec_x = filt_thrvec_x.apply(R13);
            thrvec_y = filt_thrvec_y.apply(R23);
            thrvec_z = filt_thrvec_z.apply(R33);

            //Determine wind direction by trigonometry (thrust vector tilt)
            float wind_psi = fmodf(atan2f(thrvec_y,thrvec_x),2*M_PI)*RAD_TO_DEG;

            //Get current target roll from the attitude controller
            float troll = copter.wp_nav->get_roll()/100.0f;

            //Define a dead zone around zero roll
            if(fabsf(troll) < g2.user_parameters.get_wvane_min_roll()){ last_yrate = 0; }

            //Convert roll magnitude into desired yaw rate
            float yrate = constrain_float((troll/5.0f)*g2.user_parameters.get_wvane_fine_gain(),-g2.user_parameters.get_wvane_fine_rate(),g2.user_parameters.get_wvane_fine_rate());
            last_yrate = 0.98f*last_yrate + 0.02f*yrate; //1st order LPF

            //For large compensation use "wind_psi" estimator, for fine adjusments use "yrate" estimator
            if(fabsf(troll)<g2.user_parameters.get_wvane_min_roll()){
                //Set WVANE_MIN_ROLL to zero to disable the "yrate" estimator
                //Output "y_rate" estimator
                _wind_dir = copter.cass_wind_direction/100.0f + last_yrate;
                _wind_dir = wrap_360_cd(_wind_dir*100.0f);
            }
            else{ 
                //Output "wind_psi" estimator
                _wind_dir = wrap_360_cd(wind_psi*100.0f);
                last_yrate = 0;
            }

            //Estimate wind speed with filtered parameters
            float thrvec_xy = safe_sqrt(thrvec_x*thrvec_x + thrvec_y*thrvec_y);
            _wind_speed = g2.user_parameters.get_wvane_wsA() * fabsf(thrvec_xy/thrvec_z) + g2.user_parameters.get_wvane_wsB()*safe_sqrt(fabsf(thrvec_xy/thrvec_z));
            _wind_speed = _wind_speed < 0 ? 0.0f : _wind_speed;

            //Get current velocity
            Vector3f vel_xyz = copter.inertial_nav.get_velocity(); // NEU convention
            float tyaw = copter.wp_nav->get_yaw()*DEG_TO_RAD/100.0f;
            float speed_y = vel_xyz.y*cosf(tyaw) - vel_xyz.x*sinf(tyaw);
            float speed = norm(vel_xyz.x,vel_xyz.y); 
            
            //Wind vane is active when flying horizontally steady and wind speed is perceivable
            //Condition when ascending
            if(fabsf(speed_y) < 100.0f && _wind_speed > 1.0f && vel_xyz[2] >= 0.0f){
                //Min altitude and speed at which the yaw command is sent
                if(alt>500.0f && speed<(fabsf(speed_y)+100.0f)){ 
                    //Send estimated wind direction to the autopilot
                    copter.cass_wind_direction = _wind_dir;
                    copter.cass_wind_speed = _wind_speed;
                }
                else{
                    //Send neutral values
                    copter.cass_wind_direction = copter.wp_nav->get_yaw();
                    copter.cass_wind_speed = 0.0f;
                }
            }
            //Condition when descending
            else if (fabsf(speed_y) < 100.0f && _wind_speed > 3.0f && vel_xyz[2] < 0.0f){
                if(alt>1000.0f && speed<(fabsf(speed_y)+100.0f)){ 
                    //Send estimated wind direction to the autopilot
                    copter.cass_wind_direction = _wind_dir;
                    copter.cass_wind_speed = _wind_speed;
                }
                else{
                    //Do nothing - keep yaw equal to last wind direction estimate
                    copter.cass_wind_speed = 0.0f;
                }
            }
            else{
                //Reset 1st order filter
                last_yrate = 0.0f;
            }

            //Switch to RTL automatically if wind speed is too high (in m/s)
            //If tolerance is set to zero then auto RTL is disabled but it will still warn if enabled
            if(!is_zero(g2.user_parameters.get_wvane_spd_tol())){
                if(_wind_speed > g2.user_parameters.get_wvane_spd_tol() && high_wind_flag == false && copter.flightmode->is_autopilot()){
                    gcs().send_text(MAV_SEVERITY_WARNING, "Warning high wind: Switch to RTL");
                    if(!is_zero(g2.user_parameters.get_wvane_enabled())){
                        copter.set_mode(Mode::Number::RTL, ModeReason::UNKNOWN);
                    }
                    high_wind_flag = true;
                }
                else if(_wind_speed < (g2.user_parameters.get_wvane_spd_tol() - 3.0f) && high_wind_flag == true){
                    high_wind_flag = false;
                    gcs().send_text(MAV_SEVERITY_INFO, "High wind warning cleared");
                }
            }

            //Update last loop time
            wvane_now = AP_HAL::millis();
        }
        
    }
    else{
        //Reset all global parameters to default values
        copter.cass_wind_direction = (float)copter.initial_armed_bearing;
        copter.cass_wind_speed = 0.0f;
        SRV_Channels::set_output_scaled(SRV_Channel::k_egg_drop, fan_pwm_off);
        last_yrate = 0;
        _fan_status = false;
        filt_thrvec_x.reset();
        filt_thrvec_y.reset();
        filt_thrvec_z.reset();
    }

    //Wind Data Logger ///////////////////////////////////////////////////////////////////////////////////////////

    // Write wind direction packet into the SD card
    struct log_WIND pkt_wind_est = {
        LOG_PACKET_HEADER_INIT(LOG_WIND_MSG),
        time_stamp             : AP_HAL::micros64(),
        _wind_dir              : _wind_dir/100,
        _wind_speed            : _wind_speed,
        _R13                   : R13,
        _R23                   : R23,
        _R33                   : R33
    };
    logger.WriteBlock(&pkt_wind_est, sizeof(pkt_wind_est));
}
#endif

#ifdef USERHOOK_AUXSWITCH
void Copter::userhook_auxSwitch1()
{
    // put your aux switch #1 handler here (CHx_OPT = 47)
    // Code runs when switch is toggled HIGH (pwm > 1800)
    AP_Mission::Mission_Command cmd;
    int32_t vp_lat = copter.current_loc.lat; // ahrs.get_home().lat;
    int32_t vp_lng = copter.current_loc.lng; // ahrs.get_home().lng;
    float max_alt = g2.user_parameters.get_autovp_max_alt()*100; //convert to cm

    // Check if drone is grounded and ready to create a mission
    if(ap.land_complete && copter.position_ok() && (AP_HAL::millis() - mission_now) > 5000){

        // clear mission
        copter.mode_auto.mission.clear();
        //mission.clear();

        // Command #0 : home
        cmd.id = MAV_CMD_NAV_WAYPOINT;
        cmd.content.location = Location{
                                    vp_lat,
                                    vp_lng,
                                    0,
                                    Location::AltFrame::ABOVE_HOME};
        // cmd.content.location.relative_alt = 1;
        // cmd.content.location.alt = 0;
        // cmd.content.location.lat = vp_lat;
        // cmd.content.location.lng = vp_lng;
        if (!copter.mode_auto.mission.add_cmd(cmd)) {
            gcs().send_text(MAV_SEVERITY_WARNING, "AutoVP: failed to create mission");
        }

        // Command #1 : take-off to 5m
        cmd.id = MAV_CMD_NAV_TAKEOFF;
        cmd.p1 = 0;
        cmd.content.location = Location{
                                    0,
                                    0,
                                    500,
                                    Location::AltFrame::ABOVE_HOME};
        // cmd.content.location.relative_alt = 1;
        // cmd.content.location.alt = 500; //in cm
        // cmd.content.location.lat = 0;
        // cmd.content.location.lng = 0;
        if (!copter.mode_auto.mission.add_cmd(cmd)) {
            gcs().send_text(MAV_SEVERITY_WARNING, "AutoVP: failed to create mission");
        }

        // Command #2 : Bottom waypoint at 10m
        cmd.id = MAV_CMD_NAV_WAYPOINT;
        cmd.p1 = 0;
        cmd.content.location = Location{
                                    vp_lat,
                                    vp_lng,
                                    1000,
                                    Location::AltFrame::ABOVE_HOME};
        // cmd.content.location.relative_alt = 1;
        // cmd.content.location.alt = 1000;
        // cmd.content.location.lat = vp_lat;
        // cmd.content.location.lng = vp_lng;
        if (!copter.mode_auto.mission.add_cmd(cmd)) {
            gcs().send_text(MAV_SEVERITY_WARNING, "AutoVP: failed to create mission");
        }

        // Constrain target altitude
        if(max_alt > 180000){
            max_alt = 180000;
            gcs().send_text(MAV_SEVERITY_INFO, "AutoVP: Max Alt set to 1800m");
        }
        if(max_alt < 1000){
            max_alt = 1000;
            gcs().send_text(MAV_SEVERITY_INFO, "AutoVP: Max Alt set to 10m");
        }

        // Command #3 : Top waypoint at desired altitude (from parameter)
        cmd.id = MAV_CMD_NAV_WAYPOINT;
        cmd.p1 = 0;
        cmd.content.location = Location{
                                    vp_lat,
                                    vp_lng,
                                    (int32_t)max_alt,
                                    Location::AltFrame::ABOVE_HOME};
        // cmd.content.location.relative_alt = 1;
        // cmd.content.location.alt = (int32_t)max_alt;
        // cmd.content.location.lat = vp_lat;
        // cmd.content.location.lng = vp_lng;
        if (!copter.mode_auto.mission.add_cmd(cmd)) {
            gcs().send_text(MAV_SEVERITY_WARNING, "AutoVP: failed to create mission");
        }

        // Command #4 : RTL
        cmd.id = MAV_CMD_NAV_RETURN_TO_LAUNCH;
        cmd.p1 = 0;
        cmd.content.location = Location{
                                    0,
                                    0,
                                    0,
                                    Location::AltFrame::ABOVE_HOME};
        // cmd.content.location.relative_alt = 1;
        // cmd.content.location.lat = 0;
        // cmd.content.location.lng = 0;
        // cmd.content.location.alt = 0;
        if (!copter.mode_auto.mission.add_cmd(cmd)) {
            gcs().send_text(MAV_SEVERITY_WARNING, "AutoVP: failed to create mission");
        }

        // Send successful creation message
        gcs().send_text(MAV_SEVERITY_INFO, "AutoVP mission received");
        gcs().send_text(MAV_SEVERITY_INFO, "Target alt: %f m",max_alt/100);

        mission_now = AP_HAL::millis();
    }
    else{
        // Send unable to create mission message warning
        gcs().send_text(MAV_SEVERITY_WARNING, "AutoVP: Unable to create mission");
    }
}

void Copter::userhook_auxSwitch2()
{
    // put your aux switch #2 handler here (CHx_OPT = 48)
}

void Copter::userhook_auxSwitch3()
{
    // put your aux switch #3 handler here (CHx_OPT = 49)
}
#endif
