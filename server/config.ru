$ROOT = File.expand_path(File.dirname(__FILE__))
$:.unshift $ROOT

require 'sinatra'
require 'yaml'
require 'puma'
require 'redis'
require 'jwt'
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

  # Command Struct Field Types:
  # id - Fixnum, task id.
  # command - Symbol
  # type - Symbol/String
  # freq - Fixnum/String
  # metric - Symbol/String
  # options - Array of strings already in backend format.
  CommandStruct = Struct.new(:id, :command, :type, :freq, :metric, :options)

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

run Notgios::Server
