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

      def cd(dir = '', prettify = true)
        run_cmd("cd #{dir}", prettify)
      end

      def ls(args = '', prettify = true)
        run_cmd("ls #{args}", prettify)
      end

      def git(subcommand, args = '', prettify = true)
        run_cmd("git #{subcommand} #{args}", prettify)
      end

      def run_cmd(cmd, prettify = true)
        write(cmd)
        prettify ? read.split("\n") : read
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
