require 'open3'

module Notgios
  module SSH

    AuthenticationError = Class.new(StandardError)

    class Session

      def initialize(host, port, user)
        @in, @out, @wait = Open3.popen2e('ssh', "-p #{port}", "#{user}@#{host}")
        resp = read
        resp = read while resp != ''
        write('ls')
        if read == ''
          @in.close
          @out.close
          raise AuthenticationError, 'SSH handshake failed, check keys'
        end
      end

      def cd(dir = '', pretty: true)
        run_cmd("cd #{dir}", pretty)
      end

      def ls(args = '', pretty: true)
        run_cmd("ls #{args}", pretty)
      end

      def git(subcommand, args = '', pretty: true)
        run_cmd("git #{subcommand} #{args}", pretty)
      end

      def run_cmd(cmd, timeout: 0.5, pretty: true)
        write(cmd)
        pretty ? read(timeout).split("\n") : read(timeout)
      end

      def terminate
        exited = false
        until exited
          begin
            write('exit')
            read
          rescue Errno::EPIPE, EOFError
            exited = true
          end
        end
        @in.close
        @out.close
        @wait.value
      end

      private

      def write(cmd)
        @in.puts(cmd)
      end

      def read(timeout = 0.5)
        buf = String.new
        while IO.select([@out], nil, nil, timeout)
          buf += @out.read_nonblock(10)
        end
        buf
      end

    end
  end
end
