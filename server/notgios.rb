module Notgios
  class Server < Sinatra::Application

    # Sinatra Configuration
    set :port, SERVER_PORT
    set :static, true
    set :public_folder, File.join($ROOT, 'assets')
    set :bind, '0.0.0.0'

    # Base route.
    get(/\/|\/servers|\/tasks|\/alarms|\/contacts/) do
      erb :index
    end

  end
end
