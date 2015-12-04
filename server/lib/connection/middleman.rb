# FIXME: Set this up in non-relative manner.
require_relative 'socket'
require 'thread'
require 'redis'
require 'json'

module Notgios
  module Connection

    # Command Struct Field Types:
    # id - Fixnum
    # command - Symbol
    # type - Symbol/String
    # freq - Fixnum/String
    # metrix - Symbol/String
    # options - Array of strings already in backend format.
    CommandStruct = Struct.new(:id, :command, :type, :freq, :metric, :options)

    # Monitor Struct Field Types:
    # socket - NotgiosSocket
    # tasks - Array of CommandStructs
    # queue - Queue
    # address - String
    # redis - Redis
    MonitorStruct = Struct.new(:socket, :tasks, :queue, :address, :redis)

    # Error Struct Field Types:
    # id - Fixnum
    # cause - String
    # severity - Symbol
    ErrorStruct = Struct.new(:id, :cause, :severity)

    class MiddleMan

      # FIXME: Need to set up a proper logger here.
      def initialize(listen_port, tasks, redis_host, redis_port, logger)
        @listen_socket = NotgiosSocket.new(port: listen_port)
        @connections, @connection_lock = Hash.new, Mutex.new
        @monitor_handlers = Hash.new
        @dead_handlers = Queue.new
        @error_queue = Queue.new
        @redis = Redis.new(host: redis_host, port: redis_port)
        @logger = logger
        @logger.info('MiddleMan: And so it begins...')

        # I don't think the MiddleMan should be connecting to SQL directly, after all, it's the middle man.
        # Therefore, server will provide us with all currently existing tasks during startup.
        tasks.each_pair { |monitor_address, commands| @connections[monitor_address] = MonitorStruct.new(nil, commands, Queue.new, nil, Redis.new(host: redis_host, port: redis_port)) }

        start_listening_thread
        #start_supervisor_thread
      end

      # Function used by server to send a command to a monitor.
      def enqueue_command(address, command)
        @logger.debug('MiddleMan: Enqueuing a command from the server...')
        @connection_lock.synchronize { @conntions[address].queue.push(command) }
      end

      # Function used by server to read any errors sent by monitors.
      def dequeue_error
        @logger.debug('MiddleMan: Dequeuing a reported error for the server...')
        @error_queue.pop unless @error_queue.empty?
      end

      private

      CONNECT_TIME = 1

      # Monitor will assume server is dead if MiddleMan fails to send a keepalive for more
      # than double the expected period. Set the supervisor sleep period to half the period
      # to make sure this can't happen.
      SUPERVISOR_SLEEP_PERIOD = KEEPALIVE_TIME / 2

      def send_command(socket, cmd, tasks = nil)
        valid = true

        # Send command to monitor.
        # Don't handle potential exceptions from write, let our calling method handle it.
        case cmd.command
        when :add
          @logger.debug('MiddleMan Handler: Sending an add command...')
          socket.write([
            "NGS JOB ADD",
            "ID #{cmd.id}",
            "TYPE #{cmd.type}",
            "METRIC #{cmd.metric}",
            "FREQ #{cmd.freq}"
          ].concat(cmd.options))
          tasks.push(cmd) if tasks.exists?
        when :pause
          @logger.debug('MiddleMan Handler: Sending a pause command...')
          socket.write([
            "NGS JOB PAUS",
            "ID #{cmd.id}"
          ])
        when :resume
          @logger.debug('MiddleMan Handler: Sending a resume command...')
          socket.write([
            "NGS JOB RES",
            "ID #{cmd.id}"
          ])
        when :delete
          @logger.debug('MiddleMan Handler: Sending a delete command...')
          socket.write([
            "NGS JOB DEL",
            "ID #{cmd.id}"
          ])
        else
          # Invalid task was found, scream about it so someone will notice.
          # FIXME: Need to have a proper logger to write this to.
          @logger.error('MiddleMan Handler: Received an invalid command...')
          valid = false
        end

        # Get the response from the monitor.
        if valid
          # Don't bother handling the potential exception here, let our calling method take
          # care of it.
          message = socket.read
          if message.first == 'NGS NACK'
            # NACKs can only be sent during adds, so something went wrong. Enqueue it to let the
            # server know.
            @logger.error('MiddleMan Handler: Received a NACK for command, enqueuing...')
            cause = message[1].scan(/\w+/)[1]
            @error_queue.push(ErrorStruct.new(cmd.id, cause, :nack))
          else
            @logger.debug('MiddleMan Handler: Received ACK for command...')
          end
        end
      end

      # Method handles sending metrics to their appropriate list in Redis, and responding to
      # any errors.
      def handle_monitor_message(message, monitor, redis)
        unless message.first == 'NGS JOB REPORT'
          # We've received an invalid job report. Shouldn't ever happen, but hey, the Apollo 13
          # near catastrophe was caused by a supposedly impossible quadruple failure on the regulator
          # of an oxygen canister. Shit happens. Log it, move on.
          @logger.error('MiddleMan Handler: Received an in invalid job report...')
          return
        end

        id = message[1].scan(/\d+/).first.to_i
        if message[2].index('FATAL').exists? || message[2].index('ERROR').exists?
          # We've encountered an error. Push it onto the error queue, remove if necessary, and move on.
          if message[2].index('FATAL').exists?
            severity = :fatal
            monitor.tasks.delete_if { |task| task.id == id }
            @logger.error("MiddleMan Handler: Monitor sent a FATAL message for task #{id}, removing task...")
          else
            severity = :error
            @logger.error("MiddleMan Handler: Monitor sent an ERROR message for task #{id}, enqueuing...")
          end
          cause = message[2].scan(/\w+/)[2]
          @error_queue.push(ErrorStruct.new(id, cause, severity))
        else
          # We have a valid report, push the results onto the appropriate Redis list.
          @logger.debug('MiddleMan Handler: Received a valid job report, enqueuing...')
          redis.lpush("notgios.reports.#{id}", message.slice(2..-1).to_json)
        end
      end

      # This method starts the individual monitor handling threads.
      # These threads handle all communication with the monitors after the initial handshake
      # is completed.
      # FIXME: Could current run into problems here. If the monitor goes down and comes back up
      # fast enough, it's possible this could leak a thread. Maybe. Not sure. Don't know enough
      # about sockets. Although, I suppose it doesn't matter that much because the socket would
      # be closed...
      # Synchronization on monitor_handlers needs to be revisited in general.
      def start_monitor_handler(monitor)
        handler = Thread.new do
          redis = monitor.redis
          socket = monitor.socket
          tasks = monitor.tasks
          command_queue = monitor.queue
          keepalive_timer = Time.now
          begin
            # Spin until the server initiates an orderly shutdown, or until we encounter a socket
            # error.
            until Thread.current[:should_halt].exists?
              if Time.now - keepalive_timer >= KEEPALIVE_TIME
                # It's time to send a keepalive message!
                @logger.debug('MiddleMan Handler: Sending keepalive message...')
                socket.write('NGS STILL THERE?')

                # Conceptually this spins until it receives a keepalive response or gives up,
                # but I'm putting the condition here for shutdown purposes.
                until Thread.current[:should_halt].exists?
                  message = socket.read(KEEPALIVE_TIME)
                  if message.first.exists? && message.first == 'NGS STILL HERE!'
                    @logger.debug('MiddleMan Handler: Received keepalive response...')
                    keepalive_timer = Time.now
                    break
                  elsif message.first.exists?
                    handle_monitor_message(message, monitor, redis)
                  else
                    # Monitor failed to send keepalive, continue under the assumption that it's dead.
                    @logger.error('MiddleMan Handler: Monitor failed to send keepalive, exiting...')
                    raise SocketClosedError
                  end
                end
              else
                # Check if any commands have been enqueued for us. If this were very performance
                # intensive I should really set up a signaling system so that the handler could
                # block on the queue but still send keepalive messages, but its not, so I won't
                # bother. As long as commands get sent within 10 seconds I'm sure people will be
                # fine with it.
                send_command(socket, command_queue.pop, tasks) until command_queue.empty?
                message = socket.read((10 - (Time.now - keepalive_timer)).abs)
                handle_monitor_message(message, monitor, redis) unless message.empty?
              end
            end

            # We can only reach this point if the server initiated an orderly shutdown.
            # Let the monitor know.
            @logger.debug('MiddleMan Handler: Beginning orderly shutdown...')
            socket.write('NGS BYE')
            socket.close
            @dead_handlers.push(handler)
          rescue SocketClosedError
            @logger.error('MiddleMan Handler: Encountered an error with the monitor socket, exiting...')
            socket.close
            monitor.socket = nil
            @monitor_handlers.delete(monitor.address)
            @dead_handlers.push(handler)
          end
        end
        @monitor_handlers[monitor.address] = handler
      end

      # Method starts the thread responsible for listening for new connections from monitors.
      def start_listening_thread
        @listen_thread = Thread.new do
          loop do
            socket = @listen_socket.accept
            message = socket.read(CONNECT_TIME)
            next if message.empty?

            @logger.info('MiddleMan: Accepted a connection from a new monitor...')

            ip_address = socket.peeraddr.last
            @connection_lock.synchronize do
              if @connections[ip_address].nil? || @connections[ip_address].socket.nil? || message.first == 'NGS HELLO AGAIN'
                # We have a valid new, or reopening, connection.
                @connections[ip_address] ||= MonitorStruct.new(nil, Array.new, Queue.new)
                monitor = @connections[ip_address]
                monitor.address = ip_address

                # Parse out the passed port.
                # TODO: Have to remember to enforce this > 1024 rule in the frontend.
                port = message[1].scan(/\d+/).first.to_i
                begin
                  if port > 1024
                    # We're good to go, let the monitor know.
                    socket.write('NGS ACK')
                    socket.close
                    @logger.debug('MiddleMan: Sent ACK to monitor, opening direct connection...')

                    # Open a socket to the monitor's port.
                    socket = NotgiosSocket.new(host: ip_address, port: port, tries: 2)
                    monitor.socket.close if monitor.socket.exists?
                    monitor.socket = socket

                    # Send the tasks.
                    monitor.tasks.each { |task| monitor.queue.push(task) } unless message.first == 'NGS HELLO AGAIN'
                    @logger.debug("MiddleMan: Enqueued #{monitor.tasks.size} tasks to be sent to the monitor...")

                    # Start communication thread.
                    start_monitor_handler(monitor)
                    @logger.debug('MiddleMan: Started Handler thread...')
                  else
                    @logger.debug('MiddleMan: Requested port is priviledged, sending NACK...')
                    socket.write('NGS NACK')
                    socket.close
                  end
                rescue SocketClosedError, ConnectionRefusedError
                  # We're in the initial handshake, so if the socket get closed at some point during that, just
                  # give up and start over. Doesn't matter if we accidentally call close twice.
                  @logger.error('MiddleMan: Ran into a socket error during handshake, moving on...')
                  socket.close if socket.exists?
                  next
                end
              else
                # Invalid connection. This would probably only happen if a user was intentionally
                # trying to cause trouble. Ignore exceptions.
                @logger.error('MiddleMan: Invalid handshake. Sending NACK and closing socket...')
                socket.write('NGS NACK') rescue next
                socket.close
              end
            end
          end
        end
      end

      # Monitors all other threads to make sure they stay running.
      # TODO: Take another look at this after finishing MiddleMan.
      def start_supervisor_thread
        @supervisor = Thread.new do
          until Thread.current[:should_halt].exists?
            begin
              @logger.debug('MiddleMan Supervisor: Supervising...')

              unless @listen_thread.exists? && @listen_thread.alive?
                start_listening_thread
                @logger.error('MiddleMan Supervisor: Had to restart the listening thread...')
              end

              # Make sure all of the handlers are still running.
              # Do this in two steps to ensure we don't accidentally deadlock.
              # FIXME: REVISIT THIS. I'm accessing the handler hash here without a mutex
              # because the original design could deadlock, and the listening thread is the
              # only other thread that writes into this, and I can't think of too many good
              # reasons why they should ever overlap.
              @monitor_handlers.each_pair do |address, handler|
                unless handler.exists? && handler.alive?
                  monitor = @connection_lock.synchronize { connection[address] }
                  start_monitor_handler(monitor)
                  @logger.error("MiddleMan Supervisor: Had to restart handler for monitor #{monitor.address}")
                end
              end

              # Join with all handler threads that exited for one reason or another.
              @dead_handlers.pop.join rescue nil until @dead_handlers.empty?
              @logger.debug('MiddleMan Supervisor: Joined with any dead threads...')

              sleep SUPERVISOR_SLEEP_PERIOD
            rescue
              # Supervisor is supposed to be uncrashable, so if we hit an exception
              # somewhere, just sleep for a cycle and try again.
              @logger.error('MiddleMan Supervisor: Crash prevented...')
              sleep SUPERVISOR_SLEEP_PERIOD
              retry
            end
          end
        end
      end

    end

  end
end
