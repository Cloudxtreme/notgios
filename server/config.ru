$ROOT = File.expand_path(File.dirname(__FILE__))
$:.unshift $ROOT

require 'sinatra'
require 'yaml'
require 'puma'
require 'redis'
require 'json'
require 'thread'
require 'logger'
require 'socket'
require 'digest'
require 'securerandom'
require 'lib/helpers'
require 'lib/ssh_session'
require 'lib/connection/socket'
require 'lib/connection/middleman'
require 'notgios'

module Notgios

  CONFIG = YAML::load_file(File.join($ROOT, 'config', 'notgios.yml'))
  SERVER_PORT = CONFIG['server_port']

end

run Notgios::Server
