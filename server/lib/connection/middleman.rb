# FIXME: Set this up in non-relative manner.
require_relative 'socket'
require 'thread'

module Notgios
  module Connection

    CommandStruct = Struct.new(:id, :command, :type, :freq, :metric, :options)
    MonitorStruct = Struct.new(:socket, :tasks, :queue)
    ErrorStruct = Struct.new(:id, :cause)

    class MiddleMan

      # FIXME: Need to set up a proper logger here.
      def initialize(listen_port, tasks)
        @listen_socket = NotgiosSocket.new(port: listen_port)
        @connections, @connection_lock = Hash.new, Mutex.new
        @error_queue = Queue.new

        # I don't think the MiddleMan should be connecting to SQL directly, after all, it's the middle man.
        # Therefore, server will provide us with all currently existing tasks during startup.
        tasks.each_pair { |monitor_address, commands| connections[monitor_address] = MonitorStruct.new(nil, commands, Queue.new) }

        start_listening_thread
      end

      private

      CONNECT_TIME = 1

      def send_command(socket, cmd)
        valid = true
        case cmd.command
        when :add
          socket.write([
            "NGS JOB ADD",
            "ID #{cmd.id}",
            "TYPE #{cmd.type}",
            "METRIC #{cmd.metric}",
            "FREQ #{cmd.freq}"
          ].concat(cmd.options))
        when :pause
          socket.write([
            "NGS JOB PAUS",
            "ID #{cmd.id}"
          ])
        when :resume
          socket.write([
            "NGS JOB RES",
            "ID #{cmd.id}"
          ])
        when :delete
          socket.write([
            "NGS JOB DEL",
            "ID #{cmd.id}"
          ])
        else
          # Invalid task was found, scream about it so someone will notice.
          # FIXME: Need to have a proper logger to write this to.
          valid = false
        end
      end

      # Method starts the thread responsible for listening for new connections from monitors.
      def start_listening_thread
        @listen_thread = Thread.new do
          loop do
            socket = @listen_socket.accept
            message = socket.read(CONNECT_TIME)
            next if message.empty?

            ip_address = socket.addr.last
            @connection_lock.synchronize do
              if @connections[ip_address].nil? || @connections[ip_address].socket.nil? || message.first == 'NGS HELLO AGAIN'
                # We have a valid new, or reopening, connection.
                @connections[ip_address] ||= MonitorStruct.new(nil, Array.new, Queue.new)

                # Parse out the passed port.
                # TODO: Have to remember to enforce this > 1024 rule in the frontend.
                port = message[1].scan(/\d+/).first.to_i
                if port > 1024
                  # We're good to go, let the monitor know.
                  socket.write('NGS ACK')
                  socket.close

                  # Open a socket to the monitor's port. Not sure why this would fail, but give up if it does.
                  socket = NotgiosSocket.new(host: ip_address, port: port, tries: 2) rescue next
                  @connections[ip_address].socket.close if @connections[ip_address].socket.exists?
                  @connections[ip_address].socket = socket

                  # Send the tasks.
                  @connections[ip_address].tasks { |task| send_task(socket, task) }

                  # Start communication thread.
                else
                  socket.write('NGS NACK')
                  socket.close
                end
              else
                # Invalid connection. This would probably only happen if a user was intentionally
                # trying to cause trouble.
                socket.write('NGS NACK')
                socket.close
              end
            end
          end
        end
      end

    end

  end
end
