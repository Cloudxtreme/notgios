var notgios = angular.module('notgios', ['ngRoute', 'ngCookies', 'initialValue']);

notgios.config(['$routeProvider', '$locationProvider', function ($routeProvider, $locationProvider) {
  $routeProvider.when('/', {
    templateUrl: '/templates/home.html',
    controller: 'homeController'
  }).when('/tasks', {
    templateUrl: '/templates/tasks.html',
  controller: 'taskController'
  }).when('/alarms', {
    templateUrl: '/templates/alarms.html',
  controller: 'alarmController'
  }).when('/contacts', {
    templateUrl: '/templates/contacts.html',
  controller: 'contactController'
  }).otherwise({
    redirectTo: '/'
  });

  $locationProvider.html5Mode(true);
}]);

notgios.factory('authenticated', ['$cookies', '$http', function ($cookies, $http) {
  var authentication = {};

  authentication.loggedIn = function () {
    return $cookies.get('token') != null;
  };

  authentication.getData = function (url, success, failure) {
    if ($cookies.get('token') != null) {
      $http({
        method: 'GET',
        url: url
      }).then(success, failure);
    }
  };

  authentication.sendData = function (url, data, success, failure) {
    if ($cookies.get('token') != null) {
      $http({
        method: 'POST',
        url: url,
        data: data
      }).then(success, failure);
    }
  };

  authentication.logIn = function (token) {
    if (token) {
      var tomorrow = new Date();
      tomorrow.setDate(tomorrow.getDate() + 1);
      $cookies.put('token', token, { expires: tomorrow });
    }
  };

  authentication.logOut = function () {
    $cookies.remove('token');
  };

  return authentication;
}]);

notgios.controller('homeController', ['$scope', '$interval', 'authenticated', function ($scope, $interval, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;
  $scope.serverData = {};
  $scope.server = {};

  $scope.dataInterval = $interval(function dataInterval() {
    authenticated.getData('get_servers', function success(response) {
      for (var i = 0; i < response.data.connectedServers.length; i++) {
        server = response.data.connectedServers[i];
        if (server.lastSeen) server.lastSeen = new Date(server.lastSeen);
        else server.lastSeen = 'Never';
      }
      for (var i = 0; i < response.data.disconnectedServers.length; i++) {
        server = response.data.disconnectedServers[i];
        if (server.lastSeen) server.lastSeen = new Date(server.lastSeen);
        else server.lastSeen = 'Never';
      }
      $scope.serverData = response.data;
    }, function failure(response) {
      $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
    });
  }, 1000);

  $scope.$on('$destroy', function destruct() {
    $interval.cancel($scope.dataInterval);
  });

  $scope.showServer = function (server) {
    $scope.shownServer = server;
    $scope.server.serverName = server.name;
    $scope.server.serverAddress = server.address;
    $scope.server.sshPort = server.sshPort;
  };

  $scope.saveServer = function ($event) {
    $event.stopPropagation();
    var url;
    if ($scope.shownServer) url = 'update_server';
    else url = 'add_server';
    authenticated.sendData(url, $scope.server, function success(response) {
      $('#server-modal').modal('hide');
      $scope.shownServer = null;
    }, function failure(response) {
      $scope.modalMessage = 'There was an error saving the server, please check your internet connection.';
    });
  };

  $scope.deleteServer = function (server) {
    authenticated.sendData('delete_server', server, function success(response) {
      $('#server-modal').modal('hide');
      $scope.shownServer = null;
      $scope.server = {};
    }, function failure(response) {
      $scope.modalMessage = 'There was an error saving the server, please check your internet connection.';
    });
  };

}]);

notgios.controller('taskController', ['$scope', '$interval', 'authenticated', function  ($scope, $interval, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;
  $scope.metrics = [];

  $scope.dataInterval = $interval(function dataInterval() {
    authenticated.getData('get_tasks', function success(response) {
      $scope.taskData = response.data;
    }, function failure(response) {
      $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
    });

    if ($scope.taskData && $scope.taskData.length > 0) {
      for (var i = 0; i < $scope.taskData.length; i++) {
        (function (num) {
          var server = $scope.taskData[num];
          for (var k = 0; k < server.tasks.length; k++) {
            (function (taskNum) {
              authenticated.getData('get_metrics/' + server.tasks[taskNum].id, function success(response) {
                $scope.metrics[server.address] = response.data
              }, function failure(response) {
                $scope.refreshMessage = 'Currently unable to contact the server, please check your connection.';
              });
            })(k);
          }
        })(i);
      }
    }
  }, 1000);

  $scope.hasOptions = function (task) {
    return !$.isEmptyObject(task.options);
  };

  $scope.$on('$destroy', function destruct() {
    $interval.cancel($scope.dataInterval);
  });

  $scope.showConfig = function (task, server) {
    if (task.type == undefined) {
      task.type = 'process';
      task.options = {};
    }
    $scope.shownTask = task;
    $scope.shownServer = server;
  };

  $scope.saveTask = function () {
    var sanitized = {};
    for (var option in $scope.shownTask.options) {
      if ($scope.shownTask.options.hasOwnProperty(option)) {
        switch ($scope.shownTask.type) {
          case 'process':
            if (option == 'keepalive' || option == 'runcmd' || option == 'pidfile') {
              sanitized[option] = $scope.shownTask.options[option];
            }
            break;
          case 'directory':
            if (option == 'path') sanitized['path'] = $scope.shownTask.options['path']
            break;
        }
      }
    }
    $scope.shownTask.options = sanitized;
    var data = JSON.parse(JSON.stringify($scope.shownTask));
    data.server = $scope.shownServer.address;

    authenticated.sendData('update_job', data, function success(response) {
      $('#config-modal').modal('hide');
    }, function failure(response) {
      $scope.taskErrorMessage = 'Currently unable to contact the server, please check your connection.';
    });
  }

  $scope.upcase = function (word) {
    if (word == undefined || word == null) return;
    var character = word.charAt(0);
    return character.toUpperCase() + word.substring(1, word.length);
  };

  $scope.showVis = function (task) {
    $scope.shownVis = task;
    $('#highcharts').highcharts({
      chart: {
        type: 'scatter',
        margin: [70, 50, 60, 80],
        events: {
          click: function (e) {
            // find the clicked values and the series
            var x = e.xAxis[0].value,
            y = e.yAxis[0].value,
            series = this.series[0];

            // Add it
            series.addPoint([x, y]);
          }
        }
      },
      title: {
        text: 'User supplied data'
      },
      subtitle: {
        text: 'Click the plot area to add a point. Click a point to remove it.'
      },
      xAxis: {
        gridLineWidth: 1,
        minPadding: 0.2,
        maxPadding: 0.2,
        maxZoom: 60
      },
      yAxis: {
        title: {
          text: 'Value'
        },
        minPadding: 0.2,
        maxPadding: 0.2,
        maxZoom: 60,
        plotLines: [{
          value: 0,
          width: 1,
          color: '#808080'
        }]
      },
      legend: {
        enabled: false
      },
      exporting: {
        enabled: false
      },
      plotOptions: {
        series: {
          lineWidth: 1,
          point: {
            events: {
              'click': function () {
                if (this.series.data.length > 1) {
                  this.remove();
                }
              }
            }
          }
        }
      },
      series: [{
        data: [[20, 20], [80, 80]]
      }]
    });
  };

}]);

notgios.controller('alarmController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('contactController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('navbarController', ['$scope', '$http', '$route', 'authenticated', function ($scope, $http, $route, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;

  $scope.isActive = function (path) {
    if ($route.current && $route.current.regexp) return $route.current.regexp.test(path);
    return false;
  }

  $scope.login = function ($event) {
    $event.stopPropagation();
    if ($scope.user && $scope.pass) {
      $http({
        method: 'POST',
        url: 'sign_in',
        data: {
          username: $scope.user,
          password: $scope.pass
        }
      }).then(function success(response) {
        $scope.submissionError = '';
        authenticated.logIn(response.data);
      }, function failure(response) {
        if (response.status == 400) $scope.loginError = 'User does not exist.'
        else $scope.loginError = 'Password is incorrect';
      });
    } else {
      $scope.loginError = 'Please enter both a username and a password.';
    }
  };

  $scope.logout = function () {
    authenticated.logOut();
  };

}]);

notgios.controller('signupController', ['$scope', '$http', 'authenticated', function ($scope, $http, authenticated) {

  $scope.dropdown = 'Phone Number';

  $scope.loggedIn = authenticated.loggedIn;

  $scope.setDropdown = function (value) {
    $scope.dropdown = value;
    if (value == 'Phone Number') $scope.contactType = 'number';
    else $scope.contactType = 'email';
  }

  // I should have Angular do the form validation for me, but I don't have time to figure it out right now.
  $scope.signUp = function () {
    if ($scope.username && $scope.username.length > 0 && $scope.password && $scope.password.length > 0) {
      if ($scope.password == $scope.passConfirm && $scope.contact && $scope.contact.length > 0) {
        $http({
          method: 'POST',
          url: '/sign_up',
          data: {
            username: $scope.username,
            password: $scope.password,
            contact: $scope.contact
          }
        }).then(function success(response) {
          $scope.submissionError = '';
          $('#signup-modal').modal('hide');
          authenticated.logIn(response.data);
        }, function failure(response) {
          if (response.status == 400) $scope.submissionError = 'A user with that name already exists. Please pick another';
          else $scope.submissionError = 'There was an error during submission. Please check your internet and try again.';
        });
      } else {
        $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
      }
    } else {
      $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
    }
  };

}]);

notgios.directive('serverTable', function () {
  return {
    templateUrl: '/templates/server_table.html',
  replace: true,
  scope: {
    servers: "=servers",
  status: "=connected",
  showServer: "=callback",
  title: "=header"
  }
  };
});

notgios.directive('taskTable', function () {
  return {
    templateUrl: '/templates/task_table.html',
  replace: true,
  scope: {
    taskData: "=tasks",
  showVis: "=callback"
  }
  };
});
