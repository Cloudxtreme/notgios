require_relative 'middleman'
require 'byebug'

include Notgios::Connection

commands = {
  '159.203.119.88' => [
    CommandStruct.new(5, :add, 'process', 5, 'cpu', ['keepalive true', '/root/working/a.pid', '/root/working/a.out'])
  ]
}

byebug
m = MiddleMan.new(1234, commands, '127.0.0.1', 6379)

puts 'hello'
