//***************************************************************************
// Copyright 2007-2017 Universidade do Porto - Faculdade de Engenharia      *
// Laboratório de Sistemas e Tecnologia Subaquática (LSTS)                  *
//***************************************************************************
// This file is part of DUNE: Unified Navigation Environment.               *
//                                                                          *
// Commercial Licence Usage                                                 *
// Licencees holding valid commercial DUNE licences may use this file in    *
// accordance with the commercial licence agreement provided with the       *
// Software or, alternatively, in accordance with the terms contained in a  *
// written agreement between you and Universidade do Porto. For licensing   *
// terms, conditions, and further information contact lsts@fe.up.pt.        *
//                                                                          *
// European Union Public Licence - EUPL v.1.1 Usage                         *
// Alternatively, this file may be used under the terms of the EUPL,        *
// Version 1.1 only (the "Licence"), appearing in the file LICENCE.md       *
// included in the packaging of this file. You may not use this work        *
// except in compliance with the Licence. Unless required by applicable     *
// law or agreed to in writing, software distributed under the Licence is   *
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF     *
// ANY KIND, either express or implied. See the Licence for the specific    *
// language governing permissions and limitations at                        *
// http://ec.europa.eu/idabc/eupl.html.                                     *
//***************************************************************************
// Author: Pedro Gonçalves                                                  *
//***************************************************************************

// ISO C++ 98 headers.
#include <queue>
#include <cstring>
#include <string>
#include <iostream>
#include <cassert>
#include <stdexcept>
#include <stdio.h>

// DUNE headers.
#include <DUNE/DUNE.hpp>

//FlyCapture headers
#include <flycapture/FlyCapture2.h>

#include <exiv2/exiv2.hpp>

#include <Vision/PointGrey/SaveImage.hpp>

namespace Vision
{
  namespace PointGrey
  {
    using DUNE_NAMESPACES;

    static const int c_number_task_thread = 8;
    static const int c_time_to_release_cached_ram = 60;
    static const float c_time_to_release_camera = 5.0;
    static const float c_time_to_update_cnt_info = 3.0;

    //! %Task arguments.
    struct Arguments
    {
      //! LED scheme.
      std::string led_type;
      //! Copyright Image
      std::string copyright;
      //! Lens Model
      std::string lens_model;
      //! Lens Maker
      std::string lens_maker;
      //! Saved Image Dir
      std::string save_image_dir;
      //! Number of frames/s
      int number_fs;
    };

    //! Device driver task.
    struct Task: public DUNE::Tasks::Task
    {
      //! Configuration parameters
      Arguments m_args;
      //!
      FlyCapture2::Camera m_camera;
      //!
      FlyCapture2::CameraInfo m_camInfo;
      //!
      FlyCapture2::Error m_error;
      //!
      FlyCapture2::Image m_rawImage;
      //!
      FlyCapture2::Image m_rgbImage;
      //!
      int m_lat_deg;
      //!
      int m_lat_min;
      //!
      double m_lat_sec;
      //!
      int m_lon_deg;
      //!
      int m_lon_min;
      //!
      double m_lon_sec;
      //!
      char m_text_exif_timestamp[16];
      //!
      std::string m_back_epoch;
      //!
      std::string m_back_time;
      //!
      std::string m_path_image;
      //!
      std::string m_back_path_image;
      //!
      Path m_log_dir;
      //!
      Time::Counter<float> m_cnt_test;
      //!
      Time::Counter<float> m_clean_cached_ram;
      //!
      Time::Counter<float> m_update_cnt_frames;
      //!
      int m_thread_cnt;
      //!
      long unsigned int m_frame_cnt;
      //!
      SaveImage *m_save[c_number_task_thread];
      //!
      std::string m_note_comment;

      Task(const std::string& name, Tasks::Context& ctx):
        Tasks::Task(name, ctx),
        m_log_dir(ctx.dir_log)
      {
        param("Led Mode", m_args.led_type)
    	  .description("Led type mode.");

        param("Copyright", m_args.copyright)
        .description("Copyright of Image.");

        param("Lens Model", m_args.lens_model)
        .description("Lens Model of camera.");

        param("Lens Make", m_args.lens_maker)
        .description("Lens builder/maker.");

        param("Saved Images Dir", m_args.save_image_dir)
        .defaultValue("Photos")
        .description("Saved Images Dir.");

        param("Number Frames/s", m_args.number_fs)
        .visibility(Tasks::Parameter::VISIBILITY_DEVELOPER)
        .defaultValue("4")
        .minimumValue("1")
        .maximumValue("6")
        .description("Number Frames/s.");

        bind<IMC::EstimatedState>(this);
        bind<IMC::LoggingControl>(this);
      }

      void
      onUpdateParameters(void)
      {
      }

      void
      onResourceInitialization(void)
      {
        set_cpu_governor();

        if(m_args.number_fs > 0 && m_args.number_fs <= 6)
          m_cnt_test.setTop((1.0/m_args.number_fs));
        else
          m_cnt_test.setTop(0.5);

        m_thread_cnt = 0;
        m_frame_cnt = 0;

        char text[8];
        for(int i = 0; i < c_number_task_thread; i++)
        {
          sprintf(text, "thr%d", i);
          m_save[i] = new SaveImage(this, text);
          m_save[i]->start();
        }

        m_clean_cached_ram.setTop(c_time_to_release_cached_ram);
        m_update_cnt_frames.setTop(c_time_to_update_cnt_info);
      }

      void
      onResourceRelease(void)
      {
        Delay::wait(c_time_to_release_camera);

        for(int i = 0; i < c_number_task_thread; i++)
        {
          if (m_save[i] != NULL)
          {
            m_save[i]->stopAndJoin();
            delete m_save[i];
            m_save[i] = NULL;
          }
        }

        if(m_camera.IsConnected())
        {
          m_error = m_camera.StopCapture();
          if ( m_error != FlyCapture2::PGRERROR_OK )
            err("Erro stopping camera capture: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());

          m_error = m_camera.Disconnect();
          if ( m_error != FlyCapture2::PGRERROR_OK )
            err("Erro disconnecting camera: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
        }
      }

      void
      consume(const IMC::LoggingControl* msg)
      {
        if(msg->getSource() != getSystemId())
          return;

        if (msg->op == IMC::LoggingControl::COP_STARTED)
        {
          m_log_dir = m_ctx.dir_log / msg->name / m_args.save_image_dir;
          m_back_path_image = m_log_dir.c_str();
          m_log_dir.create();
        }
      }

      //! Consume message IMC::GpsFix
      void
      consume(const IMC::EstimatedState* msg)
      {
        if(msg->getSource() != getSystemId())
          return;

        Angles::convertDecimalToDMS(Angles::degrees(msg->lat), m_lat_deg, m_lat_min, m_lat_sec);
        Angles::convertDecimalToDMS(Angles::degrees(msg->lon), m_lon_deg, m_lon_min, m_lon_sec);
        m_note_comment = "Depth: "+to_string(msg->depth)+" m # Altitude: "+to_string(msg->alt)+" m";
      }

      int
      set_cpu_governor(void)
      {
        char buffer[16];
        char governor[16];
        std::string result = "";
        FILE* pipe = popen("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
        if (!pipe)
        {
          err("popen() failed!");
          setEntityState(IMC::EntityState::ESTA_ERROR, Status::CODE_INTERNAL_ERROR);
        }
        else
        {
          std::memset(&buffer, '\0', sizeof(buffer));
          try
          {
            while (!feof(pipe))
            {
              if (fgets(buffer, sizeof(buffer), pipe) != NULL)
                result += buffer;
            }
          }
          catch (...)
          {
            pclose(pipe);
            throw;
          }
          pclose(pipe);
          std::sscanf(buffer, "%s", governor);
          if( std::strcmp(governor, "ondemand") == 0)
          {
            inf("CPU governor is already ondemand");
          }
          else
          {
            war("CPU governor is not in ondemand, setting to ondemand");
            return std::system("echo ondemand > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
          }
        }

        return -1;
      }

      void
      updateStrobe(void)
      {
        if (m_args.led_type == "STROBE")
        {
          war("enabling strobe output");
          return;
        }
        else if (m_args.led_type == "ON")
        {
          war("leds always on");
          return;
        }
        else
        {
          war("leds always off");
        }
      }

      // Check for the presence of feature
      bool
      checkValueOfCamera( FlyCapture2::Camera* pCam , unsigned int feature, bool isFeature)
      {
        FlyCapture2::Error error2;
        unsigned int regVal = 0;
        error2 = pCam->ReadRegister( feature, &regVal );
        if ( error2 != FlyCapture2::PGRERROR_OK )
        {
          err("erro reading feature: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
          return false;
        }

        if(isFeature)
        {
          if( ( regVal & 0x10000 ) != 0x10000 )
          {
            war("Feature not present.");
            inf("Value: %u", regVal);
            return false;
          }
          else
          {
            war("Feature present.");
            inf("Value: %u", regVal);
          }
        }
        else
        {
          inf("Value: %u", regVal);
        }

        return true;
      }

      void
      getInfoCamera(void)
      {
        debug("Vendor Name: %s", m_camInfo.vendorName);
        debug("Model Name: %s", m_camInfo.modelName);
        debug("Serial Number: %d", m_camInfo.serialNumber);
        debug("Sensor Info: %s", m_camInfo.sensorInfo);
        debug("Sensor Resolution: %s", m_camInfo.sensorResolution);
        debug("Firmware Version: %s", m_camInfo.firmwareVersion);
        debug("copyright: %s", m_args.copyright.c_str());
        debug("Lens Model: %s", m_args.lens_model.c_str());
        debug("Lens Maker: %s", m_args.lens_maker.c_str());
      }

      bool
      setUpCamera(void)
      {
        inf("Initialization of Camera");
        // Connect the camera
        m_error = m_camera.Connect( 0 );
        if ( m_error != FlyCapture2::PGRERROR_OK )
        {
          err("Failed to connect to camera: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
          return false;
        }

        // Get the camera info and print it out
        m_error = m_camera.GetCameraInfo( &m_camInfo );
        if ( m_error != FlyCapture2::PGRERROR_OK )
        {
          err("Failed to get camera info from camera: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
          return false;
        }

        m_error = m_camera.StartCapture();
        if ( m_error != FlyCapture2::PGRERROR_OK )
        {
          err("Failed to start image capture: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
          return false;
        }

        //const unsigned int k_triggerInq = 0x530;
        //checkValueOfCamera(&camera, 0x530, true);

        //set IO1 (4) Output
        /*error = camera.WriteRegister( 0x1110, 0x80080001 );
                if ( error != FlyCapture2::PGRERROR_OK )
                  err("erro writing");*/
        m_error = m_camera.WriteRegister( 0x1120, 0x80080001 );
        if ( m_error != FlyCapture2::PGRERROR_OK )
          err("erro writing");

        //current gpio config
        /*checkValueOfCamera(&camera, 0x1110, false);
                checkValueOfCamera(&camera, 0x1120, false);
                checkValueOfCamera(&camera, 0x11F8, false);*/

        getInfoCamera();
        inf("Camera ready.");
        return true;
      }

      bool
      getImage(void)
      {
        bool result = false;
        saveInfoExif();
        m_error = m_camera.RetrieveBuffer( &m_rawImage );
        if ( m_error != FlyCapture2::PGRERROR_OK )
        {
          err("capture error: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
          return false;
        }
        // convert to rgb
        m_error = m_rawImage.Convert( FlyCapture2::PIXEL_FORMAT_BGR, &m_rgbImage );
        if ( m_error != FlyCapture2::PGRERROR_OK )
        {
          err("convert error: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
          return false;
        }

        debug("Size Image Capture: %u x %u", m_rgbImage.GetCols(), m_rgbImage.GetRows());

        m_path_image = m_back_path_image.c_str();
        m_path_image.append("/");
        m_path_image.append(m_back_epoch);
        m_path_image.append(".jpg");

        result = m_save[m_thread_cnt]->saveNewImage(m_rgbImage, m_path_image);

        m_thread_cnt++;
        if(m_thread_cnt >= c_number_task_thread)
          m_thread_cnt = 0;

        m_rgbImage.ReleaseBuffer();
        m_rawImage.ReleaseBuffer();

        return result;
      }

      int
      releaseRamCached(void)
      {
        debug("Releasing ram cached.");
        return std::system("echo 3 > /proc/sys/vm/drop_caches");
      }

      void
      saveInfoExif(void)
      {
        std::memset(&m_text_exif_timestamp, '\0', sizeof(m_text_exif_timestamp));
        std::sprintf(m_text_exif_timestamp, "%0.4f", Clock::getSinceEpoch());
        m_back_epoch = m_text_exif_timestamp;

        m_save[m_thread_cnt]->m_exif_data.lat_deg = m_lat_deg;
        m_save[m_thread_cnt]->m_exif_data.lat_min = m_lat_min;
        m_save[m_thread_cnt]->m_exif_data.lat_sec = m_lat_sec;
        m_save[m_thread_cnt]->m_exif_data.lon_deg = m_lon_deg;
        m_save[m_thread_cnt]->m_exif_data.lon_min = m_lon_min;
        m_save[m_thread_cnt]->m_exif_data.lon_sec = m_lon_sec;
        m_save[m_thread_cnt]->m_exif_data.date_time_original = Time::Format::getTimeDate().c_str();
        m_save[m_thread_cnt]->m_exif_data.date_time_digitized = m_back_epoch.c_str();
        m_save[m_thread_cnt]->m_exif_data.make = m_camInfo.vendorName;
        m_save[m_thread_cnt]->m_exif_data.model = m_camInfo.modelName;
        m_save[m_thread_cnt]->m_exif_data.lens_make = m_args.lens_maker.c_str();
        m_save[m_thread_cnt]->m_exif_data.lens_model = m_args.lens_model.c_str();
        m_save[m_thread_cnt]->m_exif_data.copyright = m_args.copyright.c_str();
        m_save[m_thread_cnt]->m_exif_data.artist = getSystemName();
        m_save[m_thread_cnt]->m_exif_data.notes = m_note_comment.c_str();
      }

      template <class T>
      inline std::string to_string (const T& t)
      {
          std::stringstream ss;
          ss << t;
          return ss.str();
      }

      void
      onMain(void)
      {
        releaseRamCached();
        updateStrobe();
        try
        {
          if(!setUpCamera())
            throw RestartNeeded("Cannot detect camera", 5);

          setEntityState(IMC::EntityState::ESTA_NORMAL, "Led Mode - "+m_args.led_type+" # Fps: "+to_string(m_args.number_fs));
          m_thread_cnt = 0;
          m_cnt_test.reset();
        }
        catch(...)
        {
          throw RestartNeeded("Erro Flycapture API", 5);
        }

        while (!stopping())
        {
          consumeMessages();
          if(m_cnt_test.overflow())
          {
            m_cnt_test.reset();
            try
            {
              if(!getImage())
              {
                war("Restarting camera...");
                if(m_camera.IsConnected())
                {
                  m_error = m_camera.StopCapture();
                  if ( m_error != FlyCapture2::PGRERROR_OK )
                    err("Erro stopping camera capture: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());

                  m_error = m_camera.Disconnect();
                  if ( m_error != FlyCapture2::PGRERROR_OK )
                    err("Erro disconnecting camera: %s", m_save[m_thread_cnt]->getNameError(m_error).c_str());
                }
                setUpCamera();
              }
              else
              {
                m_frame_cnt++;
              }
            }
            catch(...)
            {
              err("Erro saving image %s", m_path_image.c_str());
            }
            trace("Capture: thr %d", m_thread_cnt);
          }

          if(m_clean_cached_ram.overflow())
          {
            m_clean_cached_ram.reset();
            releaseRamCached();
          }

          if(m_update_cnt_frames.overflow())
          {
            m_update_cnt_frames.reset();
            setEntityState(IMC::EntityState::ESTA_NORMAL, "Led Mode - "+m_args.led_type+" # Fps: "+to_string(m_args.number_fs)+" # "+to_string(m_frame_cnt));
          }
        }
      }
    };
  }
}

DUNE_TASK
