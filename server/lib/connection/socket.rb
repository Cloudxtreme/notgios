# FIXME: Set this up in a non-relative manner.
require 'socket'
require_relative '../helpers'

module Notgios
  module Connection

    KEEPALIVE_TIME = 10
    NoHostError = Class.new(StandardError)
    PortInUseError = Class.new(StandardError)
    NotSupportedError = Class.new(StandardError)
    SocketClosedError = Class.new(StandardError)
    ConnectionRefusedError = Class.new(StandardError)

    class NotgiosSocket

      def initialize(host: nil, port: nil, socket: nil, tries: 0)
        retry_counter = 0
        if socket
          @socket = socket
        else
          raise ArgumentError, 'Port cannot be nil' unless port.exists?
          @server = host.nil?
          @socket = @server ? TCPServer.new(port) : TCPSocket.new(host, port)
        end
      rescue Errno::EADDRINUSE
        raise PortInUseError, "Port #{port} has already been bound to"
      rescue Errno::ECONNREFUSED
        if tries > 0
          sleep 2 ** retry_counter
          tries -= 1
          retry_counter += 1
          retry
        else
          raise ConnectionRefusedError, 'Nobody\'s listening...'
        end
      rescue SocketError
        raise NoHostError, 'Supplied host cannot be found'
      end

      def accept(wait = true)
        raise NotSupportedError, 'Not a server socket' unless @server
        raise SocketClosedError, 'Already called close' unless @socket.exists?

        NotgiosSocket.new(socket: wait ? @socket.accept : @socket.accept_nonblock)
      rescue IO::WaitReadable
        nil
      end

      def write(msg, nonblocking = true)
        raise NotSupportedError, 'Not a writable socket' if @server
        raise SocketClosedError, 'Already called close' unless @socket.exists?

        message = String.new
        case msg
        when Array
          message += msg.take_while { |line| line.class == String }.join("\n") + "\n\n"
        when String
          message += msg + "\n\n"
        else
          raise ArgumentError, 'Object not writable'
        end
        nonblocking ? @socket.write_nonblock(message) : @socket.write(message)
      rescue Errno::EPIPE
        raise SocketClosedError, 'Socket raised an EPIPE, please call close'
      rescue IO::WaitWritable
        0
      end

      def read(timeout = KEEPALIVE_TIME * 2)
        raise NotSupportedError, 'Not a readable socket' if @server
        raise SocketClosedError, 'Already called close' unless @socket.exists?

        if IO.select([@socket], nil, nil, timeout)
          message, tmp = String.new, @socket.gets
          if tmp.exists?
            until tmp == "\n"
              message += tmp
              tmp = @socket.gets
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
      rescue IOError
        nil
      end

    end

  end
end
