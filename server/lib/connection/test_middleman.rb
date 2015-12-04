require_relative 'middleman'
require 'logger'
require 'byebug'

include Notgios::Connection

Thread.abort_on_exception = true

commands = {
  '159.203.119.88' => [
    CommandStruct.new(5, :add, 'PROCESS', 5, 'CPU', ['KEEPALIVE TRUE', 'PIDFILE /root/working/a.pid', 'RUNCMD /root/working/a.out'])
  ]
}

logger = Logger.new(STDOUT)
logger.level = Logger::DEBUG
m = MiddleMan.new(1234, commands, '127.0.0.1', 6379, logger)

sleep
