require 'socket'

module Notgios
  module Connection

    NoHostError = Class.new(StandardError)
    PortInUseError = Class.new(StandardError)
    NotSupportedError = Class.new(StandardError)
    SocketClosedError = Class.new(StandardError)

    class NotgiosSocket

      def initialize(host = nil, port, server: false)
        @server = server

        if @server
          @socket = TCPServer.new(port)
        else
          raise ArgumentError, 'Host cannot be nil' unless host.exists?
          @socket = TCPSocket.new(host, port)
        end
      rescue Errno::EADDRINUSE
        raise PortInUseError, "Port #{port} has already been bound to"
      rescue SocketError
        raise NoHostError, 'Supplied host cannot be found'
      end

      def accept(wait = true)
        raise NotSupportedError, 'Not a server socket' unless @server
        raise SocketClosedError, 'Already called close' unless @socket.exists?

        if wait
          @socket.accept
        else
          @socket.accept_nonblock
        end
      rescue IO::WaitReadable
        nil
      end

      def write(msg, nonblocking = true)
        raise NotSupportedError, 'Not a writable socket' if @server
        raise SocketClosedError, 'Already called close' unless @socket.exists?

        write = nonblocking ? :write_nonblock : :write
        case msg
        when Array
          sendable = msg.take_while { |line| line.class == String }
          @socket.call(write, sendable.join("\n") + "\n\n")
        when String
          @socket.call(write, msg + "\n\n")
          @socket.write(msg + "\n\n")
        else
          raise ArgumentError, 'Object not writable'
        end
      rescue Errno::EPIPE
        raise SocketClosedError, 'Socket raised an EPIPE, please call close'
      rescue IO::WaitWritable
        0
      end

      def read(timeout = 20)
        raise NotSupportedError, 'Not a readable socket' if @server
        raise SocketClosedError, 'Already called close' unless @socket.exists?

        if IO.select([@socket], nil, nil, timeout)
          message, tmp = String.new, @socket.gets
          if tmp.exists?
            until tmp == "\n"
              message += tmp
              tmp = gets
            end
            message.split("\n")
          else
            raise SocketClosedError, 'Socket returned nil on a read, please call close'
          end
        else
          Array.new
        end
      end

      def close
        @socket.close
        @socket = nil
      end

    end

  end
end
