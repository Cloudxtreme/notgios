$ROOT = File.expand_path(File.dirname(__FILE__))
$:.unshift $ROOT

# System Requirements
require 'yaml'
require 'json'
require 'thread'
require 'logger'
require 'socket'
require 'digest'
require 'securerandom'

# Gem Requirements
require 'sinatra'
require 'puma'
require 'redis'
require 'jwt'
require 'tilt/erb'
require 'rack/parser'
require 'byebug'

Thread.abort_on_exception = true

# Set up a parser since Angular sends post params weirdly.
use Rack::Parser, content_types: { 'application/json' => Proc.new { |body| JSON.parse(body) } }

# Constant Declaration/Setup
module Notgios

  CONFIG = YAML::load_file(File.join($ROOT, 'config', 'notgios.yml'))
  SERVER_PORT = CONFIG['server_port']
  MONITOR_LISTEN_PORT = CONFIG['monitor_listen_port']
  REDIS_HOST = CONFIG['redis_host']
  REDIS_PORT = CONFIG['redis_port']
  AUTH_SECRET = CONFIG['auth_secret'] || SecureRandom.hex(16)

  # Command Struct Field Types:
  # id - Fixnum, task id.
  # command - Symbol
  # server - String, ip address of server task is running on.
  # type - Symbol/String
  # freq - Fixnum/String
  # metric - Symbol/String
  # options - Array of strings already in backend format.
  CommandStruct = Struct.new(:id, :command, :server, :type, :freq, :metric, :options)

  # Error Struct Field Types:
  # id - Fixnum, task id.
  # cause - String
  # severity - Symbol
  ErrorStruct = Struct.new(:id, :cause, :severity) 

  # Alarm Struct Field Types:
  # id - Fixnum, task id.
  # threshold - String, point where alarm should be triggered.
  # priority - Fixnum
  # predictive - Boolean
  AlarmStruct = Struct.new(:id, :threshold, :priority, :predictive)

end

# Local Requirements
require 'lib/helpers'
require 'lib/nodis'
require 'lib/authtoken'
require 'lib/ssh_session'
require 'lib/connection/socket'
require 'lib/connection/middleman'
require 'notgios'

# Define a global instance for the MiddleMan
module Notgios
  MIDDLEMAN = Connection::MiddleMan.new(MONITOR_LISTEN_PORT, Helpers.with_nodis { |nodis| nodis.get_all_jobs })
end

run Notgios::Server
